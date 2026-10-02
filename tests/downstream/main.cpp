// Downstream consumer of the installed Disaster Recovery Coordinator package.
// It is built only against an installed prefix: if the exported target, its
// include directories, or its transitive dependencies were wrong, this program
// would not compile or link.

#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <system_error>

#include "drc/engine.hpp"
#include "drc/version.hpp"

namespace {

int fail(const std::string& message) {
    std::cerr << "consumer failure: " << message << "\n";
    return 1;
}

}  // namespace

int main() {
    std::cout << "downstream consumer against Disaster Recovery Coordinator "
              << drc::version_string() << "\n";

    std::error_code error;
    const std::filesystem::path directory =
        std::filesystem::temp_directory_path(error) / "drc-downstream-consumer";
    std::filesystem::remove_all(directory, error);
    std::filesystem::create_directories(directory, error);

    drc::CoordinatorOptions options;
    options.directory = directory.string();
    options.owner = "downstream-consumer";
    options.clock = std::make_shared<drc::ManualClock>();
    drc::Result<std::unique_ptr<drc::Coordinator>> opened = drc::Coordinator::open(options);
    if (!opened.ok()) {
        return fail("open failed: " + opened.status().to_string());
    }
    std::unique_ptr<drc::Coordinator> coordinator = std::move(opened).value();

    drc::FailureDomainRecord first_domain;
    first_domain.id = drc::FailureDomainId{1};
    first_domain.name = "consumer-domain";
    if (!coordinator->define_failure_domain(first_domain).ok()) {
        return fail("define_failure_domain failed");
    }
    drc::FailureDomainRecord second_domain;
    second_domain.id = drc::FailureDomainId{2};
    second_domain.name = "consumer-domain-2";
    if (!coordinator->define_failure_domain(second_domain).ok()) {
        return fail("define_failure_domain failed");
    }
    drc::SiteRecord site;
    site.id = drc::SiteId{1};
    site.name = "consumer-site";
    site.domain = first_domain.id;
    site.capacity_units = 100;
    if (!coordinator->define_site(site).ok()) {
        return fail("define_site failed");
    }
    drc::SiteRecord survivor;
    survivor.id = drc::SiteId{2};
    survivor.name = "consumer-survivor";
    survivor.domain = second_domain.id;
    survivor.capacity_units = 1000;
    if (!coordinator->define_site(survivor).ok()) {
        return fail("define_site failed");
    }
    drc::ProtectedObligation obligation;
    obligation.id = drc::ObligationId{1};
    obligation.name = "consumer-obligation";
    obligation.recovery_class = drc::RecoveryClass::Protected;
    obligation.home_site = site.id;
    obligation.required_capacity_units = 25;
    if (!coordinator->define_obligation(obligation).ok()) {
        return fail("define_obligation failed");
    }
    drc::DestinationCapability capability;
    capability.site = survivor.id;
    capability.generation = drc::Generation{1};
    capability.available_capacity_units = 500;
    capability.supported_classes_mask = (1u << drc::kRecoveryClassCount) - 1u;
    capability.observed_at = options.clock->now_nanos();
    capability.observation_epoch = coordinator->epoch();
    capability.source = "consumer";
    if (!coordinator->record_capability(capability).ok()) {
        return fail("record_capability failed");
    }
    for (const drc::EffectDomain domain :
         {drc::EffectDomain::SiteControlPlane, drc::EffectDomain::PlacementReservationCapacity,
          drc::EffectDomain::AsiExecutionRecovery, drc::EffectDomain::DfiNetworkRecovery}) {
        drc::EndpointDescriptor endpoint;
        endpoint.domain = domain;
        endpoint.id = drc::EndpointId{1};
        endpoint.name = "consumer-synthetic";
        if (!coordinator->register_endpoint(endpoint).ok()) {
            return fail("register_endpoint failed");
        }
    }

    drc::DisasterDeclaration declaration;
    declaration.declared_by = "consumer";
    declaration.reason = "consumer smoke test";
    declaration.affected_sites.push_back(site.id);
    drc::Result<drc::DisasterEventId> event = coordinator->declare_disaster(declaration);
    if (!event.ok()) {
        return fail("declare_disaster failed: " + event.status().to_string());
    }
    drc::CreatePlanRequest plan_request;
    plan_request.event = event.value();
    plan_request.requested_by = "consumer";
    drc::Result<drc::RecoveryPlanId> plan = coordinator->create_plan(plan_request);
    if (!plan.ok()) {
        return fail("create_plan failed: " + plan.status().to_string());
    }
    const drc::RecoveryPlan view = coordinator->plan(plan.value()).value();
    if (view.order.empty() || view.steps.size() != view.order.size()) {
        return fail("plan shape is not self-consistent");
    }
    if (!coordinator->begin_recovery(event.value()).ok()) {
        return fail("begin_recovery failed");
    }
    for (int round = 0; round < 8; ++round) {
        drc::AdvanceRequest advance;
        advance.event = event.value();
        advance.max_rounds = 4;
        drc::Result<drc::AdvanceReport> report = coordinator->advance(advance);
        if (!report.ok()) {
            return fail("advance failed: " + report.status().to_string());
        }
        if (report.value().quiescent) {
            break;
        }
    }
    const drc::DisasterEvent final_event = coordinator->event(event.value()).value();
    std::cout << "final phase: " << drc::to_string(final_event.phase)
              << ", steps: " << view.order.size()
              << ", state digest: " << coordinator->state_digest().to_hex() << "\n";
    if (final_event.phase != drc::EventPhase::Stabilized) {
        return fail("recovery did not stabilize");
    }
    if (!coordinator->shutdown().ok()) {
        return fail("shutdown failed");
    }
    std::filesystem::remove_all(directory, error);
    std::cout << "consumer ok\n";
    return 0;
}
