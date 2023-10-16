#include <filesystem>
#include <fstream>
#include <iostream>

#include "base/exception.h"
#include "base/request.h"
#include "frontend/frontend.h"

namespace Ramulator {

class GEM5 : public IFrontEnd, public Implementation {
    RAMULATOR_REGISTER_IMPLEMENTATION(IFrontEnd, GEM5, "GEM5", "GEM5 frontend.")

public:
    void init() override{};
    void tick() override{};

    bool receive_external_requests(int req_type_id, Addr_t addr, int source_id, std::function<void(Request &)> callback) override {
        return m_memory_system->send({addr, (Ramulator::Request::Type)req_type_id, source_id, callback});
    }

private:
    bool is_finished() override { return true; };
};

} // namespace Ramulator