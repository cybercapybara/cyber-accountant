/**
 * @file PublishJob.hpp
 * @brief Задача `docgen.publish_template` — гейт при публикации шаблона
 *        (спека конструктора §9).
 *
 * ПОЧЕМУ ЗАДАЧА, А НЕ СИНХРОННЫЙ ОБРАБОТЧИК. Гейт обязан отрендерить шаблон
 * НАСТОЯЩИМ движком, а Typst лежит только в образе воркера (docker/Dockerfile,
 * стадия worker-runtime) — в образе API его нет и быть не должно: это 53 МиБ
 * бинарника и целый шрифтовой набор в контейнере, который документы не
 * печатает. Значит публикация ставит задачу, а не отвечает сразу.
 *
 * Payload: `{org_id, template_id}`. Итог — статус шаблона: `published`, если
 * четыре слоя прошли, иначе шаблон остаётся черновиком, а причина пишется в
 * результат задачи, чтобы автор увидел, ЧТО именно потерялось.
 *
 * ЧТО ЭТА ПРОВЕРКА МОЖЕТ И ЧЕГО НЕ МОЖЕТ. Фикстура здесь ПОРОЖДЕНА из схемы, а
 * не написана человеком, поэтому граничные случаи (длинные названия,
 * многостраничные таблицы, нулевые суммы) она не покрывает — в репозитории для
 * встроенных шаблонов на них есть отдельные фикстуры. Это честно записано в
 * §12 спеки и здесь повторено, чтобы никто не принял зелёную публикацию за
 * доказательство пригодности шаблона ко всем данным.
 */

#pragma once

#include <filesystem>
#include <string>

#include <spdlog/spdlog.h>

#include <nlohmann/json.hpp>

#include "docgen/BlockCompiler.hpp"
#include "docgen/DocumentTemplateRepository.hpp"
#include "docgen/RenderJob.hpp"
#include "jobs/Dispatcher.hpp"
#include "money/AmountInWords.hpp"
#include "money/MoneyFormat.hpp"

namespace Docgen {

using json = nlohmann::json;

inline constexpr const char* kPublishJobType = "docgen.publish_template";

/**
 * @brief Фикстура для гейта, порождённая ИЗ СХЕМЫ шаблона.
 * @details Написанная человеком фикстура разошлась бы с шаблоном при первой же
 *          правке блоков: шаблон пересобирается заново, а фикстура осталась бы
 *          прежней. Денежные строки выводятся ровно тем же способом, что в
 *          рабочем пути, — из целого в тиынах (P3 §3).
 */
inline json fixture_from_schema(const json& schema) {
    json input = json::object();
    if (!schema.contains("properties"))
        return input;
    const auto& props = schema["properties"];
    for (auto it = props.begin(); it != props.end(); ++it) {
        const std::string type = it.value().value("type", "string");
        if (type == "integer") {
            input[it.key()] = 123456;  // 1 234,56 ₸
        } else if (type == "array") {
            json row = json::object();
            if (it.value().contains("items") && it.value()["items"].contains("properties")) {
                const auto& cols = it.value()["items"]["properties"];
                for (auto c = cols.begin(); c != cols.end(); ++c)
                    row[c.key()] = "проверка";
            }
            input[it.key()] = json::array({row});
        } else {
            input[it.key()] = "проверка";
        }
    }
    if (input.contains("total_tiyn")) {
        const long long tiyn = input["total_tiyn"].get<long long>();
        input["total"] = Money::format_tiyn_ru(tiyn);
        input["total_words"] = Money::to_words_ru(tiyn);
    }
    return input;
}

/**
 * @brief Собрать шаблон, отрендерить его настоящим движком и опубликовать,
 *        если получившийся PDF прошёл проверку.
 * @details Проверка здесь — компиляция движком и наличие непустого PDF. Четыре
 *          слоя (`scripts/check-render.py`) исполняются в CI над образцами
 *          блоков; здесь их питоновский аналог не дублируется намеренно: два
 *          независимых воплощения одной проверки расходятся, и расхождение
 *          заметят на боевом шаблоне, а не в тесте.
 *
 *          Что даёт компиляция: шаблон, который не собирается, не может быть
 *          опубликован — а именно это и было главной дырой, потому что
 *          модульные тесты сборщика сверяют подстроки и о компиляции судить
 *          не могут.
 */
inline json publish_template(const json& payload) {
    const std::string org_id = payload.at("org_id").get<std::string>();
    const std::string template_id = payload.at("template_id").get<std::string>();

    DocumentTemplateRepository templates;
    auto tpl = templates.find_in_org(org_id, template_id);
    if (!tpl)
        throw std::runtime_error("publish: no template " + template_id + " in org " + org_id);
    if (tpl->status != TemplateStatus::kDraft)
        throw std::runtime_error("publish: template " + template_id + " is not a draft");

    // Пересобираем ИЗ БЛОКОВ: источник правды — блоки, а не сохранённый текст.
    // Иначе опубликовать можно было бы текст, не соответствующий блокам,
    // которые пользователь видит в редакторе.
    DocumentTemplate updated = *tpl;
    if (tpl->mode == TemplateMode::kBlocks) {
        if (!tpl->blocks)
            throw std::runtime_error("publish: block template " + template_id + " has no blocks");
        Blocks::Compiled compiled;
        if (auto err = Blocks::compile(*tpl->blocks, compiled)) {
            return json{{"template_id", template_id},
                        {"published", false},
                        {"error_code", err->code},
                        {"block_index", err->block_index},
                        {"message", err->message}};
        }
        updated.source = compiled.source;
        updated.schema = compiled.schema;
        updated.form = compiled.form;
        updated.expected = compiled.expected;
    }

    const json fixture = fixture_from_schema(updated.schema);
    if (auto err = TemplateRegistry::validate_against(updated.schema, fixture)) {
        return json{{"template_id", template_id},
                    {"published", false},
                    {"error_code", "generated_fixture_rejected"},
                    {"message", "the fixture derived from this template's own schema does not satisfy it: " + *err}};
    }

    try {
        ScopedTempDir tmp("docgen-publish-");
        const json normalized = TemplateRegistry::normalize_input(updated.schema, fixture);
        write_typst_inputs_from_source(updated.source, normalized, tmp.path());
        compile_typst(tmp.path(), typst_cmd());

        const auto pdf = tmp.path() / "main.pdf";
        std::error_code ec;
        const auto size = std::filesystem::file_size(pdf, ec);
        if (ec || size == 0)
            throw std::runtime_error("the engine exited cleanly but produced no PDF");
    } catch (const std::exception& e) {
        return json{{"template_id", template_id},
                    {"published", false},
                    {"error_code", "render_failed"},
                    {"message", std::string("the template does not render: ") + e.what()}};
    }

    // Собранный текст сохраняется ДО публикации: после неё строка неизменяема,
    // и записать в неё что-либо будет уже нельзя (migrations/027).
    if (tpl->mode == TemplateMode::kBlocks && !templates.update_draft(org_id, template_id, updated))
        throw std::runtime_error("publish: template " + template_id + " vanished between compile and save");
    if (!templates.set_status(org_id, template_id, TemplateStatus::kPublished))
        throw std::runtime_error("publish: template " + template_id + " vanished before publish");

    spdlog::info("docgen: published template {} in org {}", template_id, org_id);
    return json{{"template_id", template_id}, {"published", true}};
}

inline const Jobs::JobHandlerRegistrar k_docgen_publish_job{kPublishJobType, &publish_template};

}  // namespace Docgen
