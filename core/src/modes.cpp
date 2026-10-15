// QO-100 DATV mode and its synthetic test signal.
#include "dect2/dvbs_rx.h"
#include "dect2/dvbs_gen.h"
#include "dect2/modes.h"
namespace dect2 {
const ModeTuning* modeTuning(int standard) {
    static const ModeTuning tuning = dvbsTuning();
    return standard == 8 ? &tuning : nullptr;
}
const ModeTuning* modeTuningById(const std::string& id) {
    return id == "dvbs" ? modeTuning(8) : nullptr;
}
double minSampleRateFor(int standard, double) {
    const auto* tuning = modeTuning(standard);
    return tuning ? tuning->minSampleRate : 0;
}
std::unique_ptr<ModeSynth> makeModeSynth(int standard, const SynthConfig& config, double rate) {
    return standard == 8 ? makeDvbsSynth(config, rate) : nullptr;
}
std::string modeSummary(const RxTelemetry& telemetry) {
    return telemetry.standard == 7 ? dvbsSummary(telemetry.dvbs) : "";
}
} // namespace dect2
