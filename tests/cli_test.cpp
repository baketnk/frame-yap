#include "cli.hpp"
#include <cassert>
#include <initializer_list>
#include <string>
#include <vector>
using namespace frameyap;
namespace {
CliOptions parse(std::initializer_list<const char*> args) {
    std::vector<const char*> argv{"frameyap"};
    argv.insert(argv.end(), args.begin(), args.end());
    return parse_cli(static_cast<int>(argv.size()), argv.data());
}
bool rejects(std::initializer_list<const char*> args) {
    try { parse(args); } catch (const CliError&) { return true; }
    return false;
}
}
int main() {
    assert(parse({}).mode == CliMode::Help);
    assert(parse({"--help"}).mode == CliMode::Help);
    assert(parse({"--version"}).mode == CliMode::Version);
    assert(parse({"--check-input", "--socket", "display-0"}).socket == "display-0");
    auto run = parse({"--run", "--assets", "/app/assets/", "--model", "/model",
                      "--worker", "/app/python/frameyap/worker.py",
                      "--python", "/python", "--threads", "4", "--head"});
    assert(run.mode == CliMode::Run && run.head && run.threads == "4" &&
           is_managed_worker(run.assets, run.worker));
    assert(asset_root("/app/assets/") == "/app");
    assert(asset_root("/app/assets") == "/app");
    assert(is_managed_worker("/app/assets/", "/app/python/frameyap/./worker.py"));
    assert(is_managed_worker("/app/assets", "/app/python/frameyap/worker.py"));
    assert(!is_managed_worker("/app/assets/", "/elsewhere/worker.py"));
    auto generic = parse({"--run", "--assets", "/assets", "--backend", "redux",
                          "--manifest-dir", "/models/manifests", "--model-store", "/models/store"});
    assert(generic.mode == CliMode::Run && generic.worker.empty() && generic.model.empty());
    assert(generic.backend == "redux" && generic.manifest_dir == "/models/manifests" &&
           generic.model_store == "/models/store");
    assert(parse({"--run", "--assets", "/assets"}).worker.empty());
    assert(parse({"--run", "--assets", "/app/assets/", "--worker", "/app/python/frameyap/worker.py",
                  "--backend", "redux"}).model.empty());
    assert(parse({"--run", "--assets", "/assets", "--worker", "/tmp/custom.py", "--model", "/weights"}).model == "/weights");
    assert(parse({"--run", "--assets", "/assets", "--backend", "a_1-"}).backend == "a_1-");
    assert(parse({"--register", "file", "--autostart"}).autostart);
    assert(parse({"--unregister", "file"}).manifest == "file");
    assert(parse({"--check-model", "redux", "--model-dir", "/tmp/m", "--json"}).json);
    assert(parse({"--list-models", "--manifest-dir", "/tmp/manifests"}).manifest_dir == "/tmp/manifests");
    for (auto args : {
             std::vector<const char*>{"--help", "--version"}, {"--record"}, {"--run", "--threads", "3"},
             {"--run", "--head", "--mount", "world"}, {"--check-input", "--assets", "/tmp"},
             {"--check-overlay", "--socket", "test"}, {"--register"}, {"--check-model", "redux"},
             {"--check-model", "--json"}, {"--list-models", "--model-dir"},
             {"--list-models", "--json", "--json"}, {"--run", "--assets", "a", "--assets", "b"},
             {"--run", "--assets", "--font"}, {"--unregister", "file", "--autostart"},
             {"--list-models", "--check-model", "redux"}, {"--run", "--model-dir", "a"},
             {"--list-models", "--backend", "redux"}, {"--check-overlay", "--model-store", "/tmp"},
             {"--run", "--assets", "a", "--worker", "custom.py"},
             {"--run", "--assets", "a", "--worker", "custom.py", "--model", "m", "--backend", "redux"},
             {"--run", "--assets", "/app/assets/", "--worker", "/other/worker.py"},
             {"--run", "--assets", "/app/assets/", "--worker", "/other/worker.py", "--backend", "redux"},
             {"--run", "--assets", "/app/assets/", "--worker", "/other/worker.py", "--model", "m", "--backend", "redux"},
             {"--run", "--assets", "/app/assets/", "--worker", "worker.py"},
             {"--run", "--worker", "/app/python/frameyap/worker.py", "--backend", "redux"},
             {"--run", "--backend", "../redux"}, {"--run", "--backend", "Redux"},
             {"--run", "--backend", "a.b"}, {"--run", "--backend", "-bad"},
             {"--run", "--backend", "abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvw"},
             {"--run", "--model-store", "relative"}, {"--run", "--manifest-dir", "relative"},
             {"--run", "--manifest-dir", "/tmp/../etc"}, {"--run", "--model-store", "/tmp/./files"},
             {"--run", "--model-store", "/tmp/\nfiles"}, {"--run", "--backend", "redux", "--backend", "redux"},
             {"--run", "--model-store"}, {"--run", "--model-store", "/store", "--model-store", "/other"},
             {"--check-model", "Redux", "--model-dir", "/models"}
         }) {
        std::vector<const char*> argv{"frameyap"};
        argv.insert(argv.end(), args.begin(), args.end());
        bool failed = false;
        try { parse_cli(static_cast<int>(argv.size()), argv.data()); }
        catch (const CliError&) { failed = true; }
        assert(failed);
    }
    assert(rejects({"--version", "extra"}));
}
