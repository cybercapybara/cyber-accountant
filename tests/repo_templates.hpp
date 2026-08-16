/**
 * @file repo_templates.hpp
 * @brief One answer, for every test bucket, to "can this test read the repo's
 *        real templates?" — and the end of a defect class.
 *
 * THE DEFECT. Three separate regression pins died the same silent death: each
 * decided whether it could run by probing for ONE ENGINE'S SOURCE FILE —
 * `templates/docs/<slug>/v1/template.tex`. The Typst migration deletes that
 * file per template, so the probe stopped finding it and the test skipped.
 * `GTEST_SKIP` is not a failure: the suite stayed green while the test ran
 * nothing. The worst of the three was
 * `MoneyFormatRu.MatchesEveryAmountDirective` (tests/unit/test_money_format
 * .cpp) — the pin that keeps printed money in the human form `450 000,00`
 * instead of the machine form `450000.00` that shipped on filed ФНО 300.00
 * forms in v0.4.2 — which skipped repo-wide the moment ФНО 300 converted.
 *
 * THE RULE. A template directory is reachable when THE DIRECTORY AND ITS
 * `schema.json` exist. Which engine compiles it is decided per version
 * directory by `Docgen::TemplateRegistry::load`, from the source file on
 * disk, and no probe may depend on that file's name. `schema.json` is the
 * right witness: it is the file every template has whatever compiles it, it
 * is what the registry needs to resolve the directory at all, and it is the
 * one that cannot go missing without the template being genuinely broken.
 * The rule outlived the migration that motivated it and is the standing one
 * for any future engine change.
 *
 * A SKIP IS LOUD, AND NARROW. The only environment that genuinely cannot run
 * these tests is one with no repo checkout under the working directory at all
 * — `templates/docs` simply is not there. That skips, with the working
 * directory named so the message is actionable instead of decorative. A
 * checkout that IS reachable but has lost the template asked for is a HARD
 * FAILURE: a renamed slug, a conversion committed one directory too high, a
 * deleted template — exactly the class of breakage that must never be
 * reported as "skipped".
 *
 * In CI neither branch is reachable: every test binary runs from `/app`, the
 * repo root (docker/Dockerfile's test-runner stage, and the `cd /app` in the
 * sanitizer and TSan jobs). `ShippedTemplatesTest
 * .TheRepoTemplateTreeIsReachableFromTheWorkingDirectory` (tests/unit/
 * test_template_registry.cpp) asserts that outright rather than skipping, so
 * a run that lost its working directory produces ONE loud red test naming the
 * cause instead of a screen of quiet green skips.
 */

#pragma once

#include <filesystem>
#include <string>

#include <gtest/gtest.h>

namespace TestTemplates {

/// The docgen template root, relative to the working directory. Still named
/// `latex` though nothing under it is LaTeX any more — it is a path in the
/// repo, not an engine claim, and renaming it is its own task.
inline constexpr const char* kRoot = "templates/docs";

/// Is a repo checkout reachable from the working directory at all? This is
/// the ONE question a skip may be based on.
inline bool checkout_reachable() {
    std::error_code ec;
    return std::filesystem::is_directory("templates", ec);
}

/// Is the docgen template root there? A checkout without it is a broken
/// repository, not a skippable environment.
inline bool tree_reachable() {
    std::error_code ec;
    return std::filesystem::is_directory(kRoot, ec);
}

/// `templates/docs/<slug>/<version>` — the directory a probe must ask about,
/// never a file inside it.
inline std::filesystem::path version_dir(const std::string& slug, const std::string& version = "v1") {
    return std::filesystem::path(kRoot) / slug / version;
}

/// Engine-agnostic reachability of one template: the directory and the
/// `schema.json` every template ships under either engine.
inline bool has_template(const std::string& slug, const std::string& version = "v1") {
    const std::filesystem::path dir = version_dir(slug, version);
    std::error_code ec;
    return std::filesystem::is_directory(dir, ec) && std::filesystem::exists(dir / "schema.json", ec);
}

/// The skip message for the one tolerable case, with the working directory in
/// it — "not reachable" without saying from WHERE is how these skips stayed
/// invisible for two releases.
inline std::string no_checkout_reason() {
    std::error_code ec;
    const std::filesystem::path cwd = std::filesystem::current_path(ec);
    return std::string("SKIPPED (no repo checkout): there is no 'templates' directory under the working directory ") +
           (ec ? std::string("<unknown>") : cwd.string()) +
           ". Run the test binary from the repo root (CI runs it from /app). This is the ONLY case in which a "
           "template-backed test may skip.";
}

/// The hard-failure message for the case that must never be a skip.
inline std::string missing_template_reason(const std::string& slug, const std::string& version = "v1") {
    return "the repo checkout is reachable but " + version_dir(slug, version).string() +
           "/schema.json is not — the template was renamed, moved or deleted. This is a broken repository, not an "
           "environment to skip over. A probe must ask for the DIRECTORY and its schema.json, never for the "
           "engine-specific source file inside it.";
}

}  // namespace TestTemplates

/// Guard for a test that sweeps the whole shipped template tree.
#define REQUIRE_REPO_TEMPLATE_TREE()                                                              \
    do {                                                                                          \
        if (!::TestTemplates::checkout_reachable())                                               \
            GTEST_SKIP() << ::TestTemplates::no_checkout_reason();                                \
        if (!::TestTemplates::tree_reachable())                                                   \
            FAIL() << "the repo checkout is reachable but '" << ::TestTemplates::kRoot            \
                   << "' is not — that is a broken repository, not an environment to skip over."; \
    } while (false)

/// Guard for a test that reads ONE shipped template. Skips only when there is
/// no checkout; FAILS when the checkout is there and the template is not.
#define REQUIRE_REPO_TEMPLATE(slug)                                   \
    do {                                                              \
        REQUIRE_REPO_TEMPLATE_TREE();                                 \
        if (!::TestTemplates::has_template(slug))                     \
            FAIL() << ::TestTemplates::missing_template_reason(slug); \
    } while (false)

/// Same contract for a repo path that is not a docgen template — the email
/// templates under `templates/email`, say.
#define REQUIRE_REPO_PATH(path)                                                                             \
    do {                                                                                                    \
        if (!::TestTemplates::checkout_reachable())                                                         \
            GTEST_SKIP() << ::TestTemplates::no_checkout_reason();                                          \
        if (!std::filesystem::exists(path))                                                                 \
            FAIL() << "the repo checkout is reachable but '" << (path)                                      \
                   << "' is not — it was renamed, moved or deleted. That is a broken repository, not an " \
                      "environment to skip over.";                                                          \
    } while (false)
