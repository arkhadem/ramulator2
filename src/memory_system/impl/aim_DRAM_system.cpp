#include "addr_mapper/addr_mapper.h"
#include "base/request.h"
#include "base/type.h"
#include "dram/dram.h"
#include "dram_controller/controller.h"
#include "memory_system/memory_system.h"
#include "translation/translation.h"
#include <cstdint>
#include <cstdio>
#include <math.h>
#include <vector>

namespace Ramulator {

#define ISR_SIZE (1 << 21)
#define MAX_CHANNEL_COUNT 8

class AiMDRAMSystem final : public IMemorySystem, public Implementation {
    RAMULATOR_REGISTER_IMPLEMENTATION(IMemorySystem, AiMDRAMSystem, "AiMDRAM", "AiM memory system (AiM DMA).");

protected:
    Clk_t m_clk = 0;
    IDRAM *m_dram;
    IAddrMapper *m_addr_mapper;
    std::vector<IDRAMController *> m_controllers;
    Logger_t m_logger;
    std::queue<Request> request_queue;
    std::queue<Request> remaining_AiM_requests[MAX_CHANNEL_COUNT];
    int AiM_req_id = 0;

    int stalled_AiM_requests = 0;

    std::function<void(Request &)> callback;

    enum class CFR {
        BROADCAST,
        EWMUL_BG,
        AFM
    };
    std::map<CFR, int32_t> CFR_values;
    std::map<Addr_t, CFR> address_to_CFR;

    uint8_t CountSetBit(const int64_t ch_mask) const {
        assert(ch_mask > 0);

        uint8_t count = 0;

        for (int i = 0; i < 32; i++)
            if (ch_mask & (0x1 << i))
                count++;

        return count;
    }

    uint8_t FindFirstChannelIndex(const uint8_t ch_mask) const {
        if ((ch_mask & 0xff) == 0)
            return 0;

        for (int i = 0; i < 32; i++)
            if (ch_mask & (0x1 << i))
                return (0x1 << i);

        return 0;
    }

    void apply_addr_mapp(Request &req, int channel_id) {
        req.addr_vec.resize(5, -1);
        req.addr_vec[0] = channel_id;
        if (req.bank_index == -1) {
            req.addr_vec[1] = -1;
            req.addr_vec[2] = -1;
        } else {
            req.addr_vec[1] = req.bank_index / 4;
            req.addr_vec[2] = req.bank_index % 4;
        }
        req.addr_vec[3] = req.row_addr;
        req.addr_vec[4] = req.col_addr;
    }

public:
    std::map<Request::Type, std::map<Request::MemAccessRegion, int>> s_num_RW_requests;
    std::map<Request::Opcode, int> s_num_AiM_requests;
    int s_ISR_queue_full = 0;
    int s_wait_RD_stall = 0;

public:
    void init() override {
        // Create device (a top-level node wrapping all channel nodes)
        m_dram = create_child_ifce<IDRAM>();
        m_addr_mapper = create_child_ifce<IAddrMapper>();

        m_logger = Logging::create_logger("AiMDRAMSystem");

        int num_channels = m_dram->get_level_size("channel");

        // Create memory controllers
        for (int i = 0; i < num_channels; i++) {
            IDRAMController *controller = create_child_ifce<IDRAMController>();
            controller->m_impl->set_id(fmt::format("Channel {}", i));
            controller->m_channel_id = i;
            m_controllers.push_back(controller);
        }

        m_clock_ratio = param<uint>("clock_ratio").required();

        // vector data for MAC is from GB (0) or next bank (1)
        address_to_CFR[0] = CFR::BROADCAST;
        CFR_values[CFR::BROADCAST] = 0;

        // EWMUL in one bank group (0) or all bank groups (1)
        address_to_CFR[1] = CFR::EWMUL_BG;
        CFR_values[CFR::EWMUL_BG] = 1;

        // Activation Function mode selects AF (0-7)
        address_to_CFR[2] = CFR::AFM;
        CFR_values[CFR::AFM] = 0;

        callback = std::bind(&AiMDRAMSystem::receive, this, std::placeholders::_1);

        register_stat(m_clk).name("memory_system_cycles");
        register_stat(s_wait_RD_stall)
            .name("total_num_wait_read_stalls")
            .desc("total number of cycles that AiM DMA is stalled because of waiting for read operations from channels");

        register_stat(s_ISR_queue_full)
            .name("total_num_ISR_full")
            .desc("total number of cycles that AiM DMA does not receive ISR because of lack of enough ISR space");

        for (const auto type : {Request::Type::Read, Request::Type::Write}) {
            for (const auto mem_access_region : {Request::MemAccessRegion::GPR, Request::MemAccessRegion::CFR, Request::MemAccessRegion::MEM}) {
                s_num_RW_requests[type][mem_access_region] = 0;
                register_stat(s_num_RW_requests[type][mem_access_region])
                    .name(fmt::format("total_num_{}_{}_requests",
                                      AiMISRInfo::convert_type_to_str(type),
                                      AiMISRInfo::convert_mem_access_region_to_str(mem_access_region)))
                    .desc(fmt::format("total number of {} {} requests",
                                      AiMISRInfo::convert_type_to_str(type),
                                      AiMISRInfo::convert_mem_access_region_to_str(mem_access_region)));
            }
        }
        for (int opcode = (int)Request::Opcode::MIN + 1; opcode < (int)Request::Opcode::MAX; opcode++) {
            s_num_AiM_requests[(Request::Opcode)opcode] = 0;
            register_stat(s_num_AiM_requests[(Request::Opcode)opcode])
                .name(fmt::format("total_num_AiM_{}_requests", AiMISRInfo::convert_AiM_opcode_to_str((Request::Opcode)opcode)))
                .desc(fmt::format("total number of AiM {} requests", AiMISRInfo::convert_AiM_opcode_to_str((Request::Opcode)opcode)));
        }
    };

    void setup(IFrontEnd *frontend, IMemorySystem *memory_system) override {}

    bool send(Request req) override {

        if (request_queue.size() == ISR_SIZE) {
            s_ISR_queue_full++;
            return false;
        }
        request_queue.push(req);
        m_logger->info("[CLK {}] {} pushed to the queue!", m_clk, req.c_str());

        switch (req.type) {
        case Request::Type::AIM: {
            s_num_AiM_requests[req.opcode]++;
            break;
        }
        case Request::Type::Read:
        case Request::Type::Write: {
            s_num_RW_requests[req.type][req.mem_access_region]++;
            break;
        }
        default: {
            throw ConfigurationError("AiMDRAMSystem: unknown request type {}!", (int)req.type);
            break;
        }
        }

        return true;
    };

    void tick() override {

        bool was_AiM_request_remaining = false;
        bool is_AiM_request_remaining = false;
        for (int channel_id = 0; channel_id < MAX_CHANNEL_COUNT; channel_id++) {
            while (remaining_AiM_requests[channel_id].empty() == false) {
                was_AiM_request_remaining = true;
                if (m_controllers[channel_id]->send(remaining_AiM_requests[channel_id].front()) == false) {
                    is_AiM_request_remaining = true;
                    break;
                }
                remaining_AiM_requests[channel_id].pop();
            }
        }

        if (stalled_AiM_requests == 0) {
            if (was_AiM_request_remaining == true) {
                if (is_AiM_request_remaining == false) {
                    Request host_req = request_queue.front();
                    if (host_req.callback)
                        host_req.callback(host_req);
                    request_queue.pop();
                }
            } else if (request_queue.empty() == false) {
                Request host_req = request_queue.front();
                m_logger->info("[CLK {}] Decoding {}...", m_clk, host_req.c_str());
                bool all_AiM_requests_sent = true;

                switch (host_req.type) {
                case Request::Type::AIM: {
                    Request::Opcode opcode = host_req.opcode;
                    auto opsize = host_req.opsize;
                    auto ch_mask = host_req.channel_mask;
                    uint8_t channel_count = CountSetBit(ch_mask);
                    Request aim_req = host_req;
                    aim_req.callback = callback;

                    switch (opcode) {

                    case Request::Opcode::ISR_WR_SBK:
                    case Request::Opcode::ISR_WR_GB:
                    case Request::Opcode::ISR_WR_BIAS:
                    case Request::Opcode::ISR_RD_MAC:
                    case Request::Opcode::ISR_RD_AF:
                    case Request::Opcode::ISR_RD_SBK:
                    case Request::Opcode::ISR_COPY_BKGB:
                    case Request::Opcode::ISR_COPY_GBBK:
                    case Request::Opcode::ISR_MAC_SBK:
                    case Request::Opcode::ISR_MAC_ABK:
                    case Request::Opcode::ISR_AF:
                    case Request::Opcode::ISR_EWMUL:
                    case Request::Opcode::ISR_WR_ABK: {
                        // Decoding opcode
                        AiMISR aim_ISR = AiMISRInfo::convert_AiM_opcode_to_AiM_ISR(opcode);

                        if (aim_ISR.channel_count_eq_one) {
                            if (channel_count != 1) {
                                throw ConfigurationError("AiMDRAMSystem: channel mask ({}) of ISR_WR_SBK must specify only 1 channel!", ch_mask);
                            }
                        }

                        if (opcode == Request::Opcode::ISR_AF) {
                            aim_req.afm = CFR_values[CFR::AFM];
                            aim_req.row_addr = (1 << 29) + aim_req.afm;
                        }

                        if ((host_req.opcode == Request::Opcode::ISR_MAC_ABK) ||
                            (host_req.opcode == Request::Opcode::ISR_MAC_SBK)) {
                            aim_req.broadcast = CFR_values[CFR::BROADCAST];
                        }

                        if (host_req.opcode == Request::Opcode::ISR_MAC_ABK) {
                            aim_req.ewmul_bg = CFR_values[CFR::EWMUL_BG];
                        }

                        if (aim_ISR.is_field_legal(AiMISR::Field::bank_index) == true)
                            aim_req.bank_index = host_req.bank_index;

                        if (aim_ISR.is_field_legal(AiMISR::Field::row_addr) == true)
                            aim_req.row_addr = host_req.row_addr;

                        if (opsize == -1)
                            opsize = 1;

                        for (int i = 0; i < opsize; i++) {
                            uint8_t channel_mask = ch_mask;

                            if (aim_ISR.is_field_legal(AiMISR::Field::col_addr) == true)
                                aim_req.col_addr = host_req.col_addr + i;

                            for (int cnt = 0; cnt < channel_count; cnt++) {
                                uint8_t channel_id = FindFirstChannelIndex(channel_mask);
                                channel_mask &= (~channel_id);
                                channel_id = log2(channel_id);

                                aim_req.AiM_req_id = AiM_req_id++;
                                aim_req.host_req_id = host_req.host_req_id;
                                apply_addr_mapp(aim_req, channel_id);
                                m_logger->info("[CLK {}] 1- Sending {} to channel {}", m_clk, aim_req.c_str(), channel_id);
                                if (m_controllers[channel_id]->send(aim_req) == false) {
                                    remaining_AiM_requests[channel_id].push(aim_req);
                                    all_AiM_requests_sent = false;
                                }

                                if (aim_ISR.AiM_DMA_blocking) {
                                    stalled_AiM_requests += 1;
                                }
                            }
                        }

                        break;
                    }

                    case Request::Opcode::ISR_WR_AFLUT: {
                        throw ConfigurationError("AiMDRAMSystem: ISR_WR_AFLUT not supported by now!");
                        break;
                    }

                    case Request::Opcode::ISR_EWADD: {
                        // Do nothing
                        break;
                    }

                    case Request::Opcode::ISR_EOC: {
                        for (int channel_id = 0; channel_id < m_controllers.size(); channel_id++) {
                            aim_req.AiM_req_id = AiM_req_id++;
                            aim_req.host_req_id = host_req.host_req_id;
                            m_logger->info("[CLK {}] 2- Sending {} to channel {}", m_clk, aim_req.c_str(), channel_id);
                            if (m_controllers[channel_id]->send(aim_req) == false) {
                                remaining_AiM_requests[channel_id].push(aim_req);
                                all_AiM_requests_sent = false;
                            }
                            stalled_AiM_requests += 1;
                        }
                        break;
                    } break;

                    default:
                        printf("unknown command \n");
                        break;
                    }
                } break;
                case Request::Type::Read: {
                    switch (host_req.mem_access_region) {
                    case Request::MemAccessRegion::CFR: {
                        // Do nothing
                        all_AiM_requests_sent = true;
                        break;
                    }
                    case Request::MemAccessRegion::GPR: {
                        // Do nothing
                        all_AiM_requests_sent = true;
                        break;
                    }
                    case Request::MemAccessRegion::MEM: {
                        Request aim_req = host_req;
                        aim_req.AiM_req_id = AiM_req_id++;
                        apply_addr_mapp(aim_req, aim_req.channel_mask);
                        int channel_id = aim_req.addr_vec[0];
                        m_logger->info("[CLK {}] 3- Sending {} to channel {}", m_clk, aim_req.c_str(), channel_id);
                        if (m_controllers[channel_id]->send(aim_req) == false) {
                            remaining_AiM_requests[channel_id].push(aim_req);
                            all_AiM_requests_sent = false;
                        }
                        stalled_AiM_requests += 1;
                        break;
                    }
                    default: {
                        throw ConfigurationError("AiMDRAMSystem: memory access region {}!", (int)host_req.mem_access_region);
                        break;
                    }
                    }
                    break;
                }
                case Request::Type::Write: {
                    switch (host_req.mem_access_region) {
                    case Request::MemAccessRegion::CFR: {
                        if (address_to_CFR.find(host_req.addr) == address_to_CFR.end()) {
                            throw ConfigurationError("AiMDRAMSystem: unknown CFR at location {}!", (int)host_req.addr);
                        }
                        CFR_values[address_to_CFR[host_req.addr]] = host_req.data;
                        all_AiM_requests_sent = true;
                        break;
                    }
                    case Request::MemAccessRegion::GPR: {
                        // Do nothing
                        all_AiM_requests_sent = true;
                        break;
                    }
                    case Request::MemAccessRegion::MEM: {
                        Request aim_req = host_req;
                        aim_req.AiM_req_id = AiM_req_id++;
                        apply_addr_mapp(aim_req, aim_req.channel_mask);
                        int channel_id = aim_req.addr_vec[0];
                        m_logger->info("[CLK {}] 4- Sending {} to channel {}, channel_mask {}", m_clk, aim_req.c_str(), channel_id, aim_req.channel_mask);
                        if (m_controllers[channel_id]->send(aim_req) == false) {
                            remaining_AiM_requests[channel_id].push(aim_req);
                            all_AiM_requests_sent = false;
                            m_logger->info("[CLK {}] 4- failed", aim_req.c_str(), m_clk, channel_id, aim_req.channel_mask);
                        } else {
                            m_logger->info("[CLK {}] 4- sent", aim_req.c_str(), m_clk, channel_id, aim_req.channel_mask);
                        }
                        break;
                    }
                    default: {
                        throw ConfigurationError("AiMDRAMSystem: memory access region {}!", (int)host_req.mem_access_region);
                        break;
                    }
                    }
                    break;
                }
                default: {
                    throw ConfigurationError("AiMDRAMSystem: unknown request type {}!", (int)host_req.type);
                    break;
                }
                }
                if ((stalled_AiM_requests == 0) && (all_AiM_requests_sent == true)) {
                    if (host_req.callback)
                        host_req.callback(host_req);
                    request_queue.pop();
                }
            }
        }

        m_clk++;
        m_dram->tick();
        for (auto controller : m_controllers) {
            controller->tick();
        }
    };

    void receive(Request &req) {
        Request host_req = request_queue.front();
        if (req.host_req_id != host_req.host_req_id)
            throw ConfigurationError("AiMDRAMSystem: received request id {} != head of the queue request id {}!", req.host_req_id, host_req.host_req_id);

        stalled_AiM_requests--;

        if (stalled_AiM_requests == 0) {
            if (host_req.callback)
                host_req.callback(host_req);
            request_queue.pop();
        }
    }

    float get_tCK() override {
        return m_dram->m_timing_vals("tCK_ps") / 1000.0f;
    }

    // const SpecDef& get_supported_requests() override {
    //   return m_dram->m_requests;
    // };
};

} // namespace Ramulator
