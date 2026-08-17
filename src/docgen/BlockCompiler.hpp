/**
 * @file BlockCompiler.hpp
 * @brief Сборка шаблона из блоков: блоки -> исходник Typst + схема + описание
 *        формы + список обязательных подписей (спека конструктора §4.1, §6, §9).
 *
 * ГЛАВНОЕ РЕШЕНИЕ — ГДЕ ОКАЗЫВАЕТСЯ ТЕКСТ АРЕНДАТОРА.
 *
 * Данные документа кодом не становятся: шаблон сам открывает `input.json`
 * (`json("input.json")`), подстановки в текст шаблона не происходит, и именно
 * поэтому в проекте нет слоя экранирования. Но у блоков есть ВТОРОЙ вид
 * пользовательского текста — статические подписи, которые арендатор пишет в
 * конструкторе («Счёт на оплату», «Вышеперечисленные работы выполнены…»). Они
 * попадают в собранный `.typ`, то есть в позицию исходника.
 *
 * Написать их в разметку нельзя: `#`, `*`, `_`, `@`, `$`, `` ` `` в разметке
 * Typst — управляющие символы, и подпись «Скидка *20%*» напечаталась бы
 * жирным, а «#panic()» исполнилась бы. Экранировать разметку — путь, который
 * уже подводил: экранирование до шаблонизации стоило ФНО 300.00 её итоговой
 * строки.
 *
 * Поэтому подписи выводятся ТОЛЬКО как СТРОКОВЫЕ ЛИТЕРАЛЫ Typst
 * (`#let s0 = "…"`), а в документ попадают через `#s0`. Строка в Typst не
 * перечитывается как разметка, а экранирования требует ровно двух символов —
 * обратной косой и кавычки, — и это полное правило, а не список, который
 * забудут дополнить. Управляющих символов разметки в позиции разметки не
 * оказывается вовсе.
 */

#pragma once

#include <cctype>
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "docgen/Variables.hpp"

namespace Docgen::Blocks {

using json = nlohmann::json;

/// Результат сборки. Всё, что нужно, чтобы положить шаблон в
/// `document_templates` и потом им отрендерить.
struct Compiled {
    std::string source;    ///< текст Typst
    json schema;           ///< контракт валидации ввода
    json form;             ///< описание формы: подписи, порядок, виджеты
    std::string expected;  ///< подписи, которые шаблон ОБЯЗАН напечатать, + поля
};

/// Ошибка сборки: что не так и в каком блоке. Возвращается вместо исключения —
/// это ожидаемый исход правки шаблона пользователем, а не сбой.
struct Error {
    std::size_t block_index = 0;
    std::string code;
    std::string message;
};

namespace detail {

/// Строковый литерал Typst. Полное правило: экранируются обратная косая и
/// кавычка, всё остальное внутри литерала буквально. Управляющие символы
/// РАЗМЕТКИ (`#`, `*`, `_`, `@`, `$`, backtick) внутри строки безвредны —
/// именно поэтому подписи и выводятся строками, а не разметкой.
inline std::string literal(const std::string& raw) {
    std::string out = "\"";
    for (const char c : raw) {
        if (c == '\\' || c == '"')
            out += '\\';
        out += c;
    }
    out += '"';
    return out;
}

/// Имя поля годно для пути в `input.json`: латиница, цифры, подчёркивание.
/// Ограничение не косметическое — имя попадает в собранный исходник как
/// `d.<имя>`, и точка или дефис в нём превратились бы в другое выражение.
inline bool is_valid_field(const std::string& name) {
    if (name.empty() || name.size() > 64)
        return false;
    if (std::isdigit(static_cast<unsigned char>(name.front())) != 0)
        return false;
    for (const char c : name) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
        if (!ok)
            return false;
    }
    return true;
}

}  // namespace detail

/// Типы блоков. Набор ЗАКРЫТ и объявлен кодом: блок, пришедший данными,
/// означал бы, что расширять шаблон можно, не проходя ни сборку, ни ревью.
namespace BlockType {
inline constexpr const char* kHeader = "header";
inline constexpr const char* kText = "text";
inline constexpr const char* kFields = "fields";
inline constexpr const char* kTable = "table";
inline constexpr const char* kTotals = "totals";
inline constexpr const char* kSignatures = "signatures";
inline constexpr const char* kPagebreak = "pagebreak";
}  // namespace BlockType

/**
 * @brief Собрать шаблон из блоков.
 * @param blocks массив блоков (источник правды, `document_templates.blocks`)
 * @return собранный шаблон либо ошибка с номером блока
 *
 * @details Преамбула собранного шаблона повторяет идиомы, выстраданные при
 *          миграции на Typst, и это не украшение:
 *          - `overhang: false` — Typst свешивает финальный дефис за поле, и
 *            гейт валит рамку слова за границу;
 *          - `hyphenate: false` — перенос РАЗРЫВАЕТ слово, и объявленная
 *            подпись перестаёт находиться как непрерывный текст; в акте это
 *            стоило потери целой фразы, причём локально дефект не
 *            воспроизводился (метрики шрифтов различаются);
 *          - `show regex("\S+/\S+")` в box — токен со слешем иначе рвётся
 *            по слешу;
 *          - линия подписи — `box` с рамкой снизу, никогда голая `line()`:
 *            нулевая высота линии дала бы линию, проведённую СКВОЗЬ текст.
 */
inline std::optional<Error> compile(const json& blocks, Compiled& out) {
    if (!blocks.is_array())
        return Error{0, "blocks_not_array", "blocks must be a JSON array"};
    if (blocks.empty())
        return Error{0, "blocks_empty", "a template needs at least one block"};
    if (blocks.size() > 200)
        return Error{0, "too_many_blocks", "a template may not have more than 200 blocks"};

    std::string body;
    std::string preamble;
    std::vector<std::string> labels;   // для expected.txt
    json properties = json::object();  // для схемы
    json required = json::array();     // для схемы
    json form_fields = json::array();  // для формы
    std::size_t literal_counter = 0;

    // Каждая подпись выводится ОДИН раз в преамбулу как строковый литерал и
    // печатается по имени. Так текст арендатора не попадает в позицию
    // разметки ни в одном месте документа.
    auto emit_label = [&](const std::string& text) {
        const std::string name = "s" + std::to_string(literal_counter++);
        preamble += "#let " + name + " = " + detail::literal(text) + "\n";
        labels.push_back(text);
        return name;
    };

    auto declare_field = [&](const std::string& name,
                             const std::string& label_ru,
                             const std::string& label_kk,
                             const std::string& type,
                             bool is_required) {
        properties[name] = json{{"type", type}};
        if (is_required)
            required.push_back(name);
        form_fields.push_back(json{
            {"field", name},
            {"label_ru", label_ru},
            {"label_kk", label_kk},
            {"widget", type == "integer" ? "money" : "text"},
        });
    };

    for (std::size_t i = 0; i < blocks.size(); ++i) {
        const json& b = blocks[i];
        if (!b.is_object() || !b.contains("type") || !b["type"].is_string())
            return Error{i, "block_type_missing", "every block needs a string 'type'"};
        const std::string type = b["type"].get<std::string>();

        if (type == BlockType::kHeader) {
            const std::string title = b.value("title", std::string{});
            if (title.empty())
                return Error{i, "header_title_required", "a header block needs a non-empty 'title'"};
            const std::string name = emit_label(title);
            body += "#align(center)[#text(size: 13pt, weight: \"bold\")[#" + name + "]]\n#v(4mm)\n";

        } else if (type == BlockType::kText) {
            const std::string text = b.value("text", std::string{});
            if (text.empty())
                return Error{i, "text_required", "a text block needs a non-empty 'text'"};
            const std::string name = emit_label(text);
            body += "#" + name + "\n#v(3mm)\n";

        } else if (type == BlockType::kFields) {
            // Пары «подпись: значение». Значение — либо переменная каталога,
            // либо поле формы.
            if (!b.contains("rows") || !b["rows"].is_array() || b["rows"].empty())
                return Error{i, "fields_rows_required", "a fields block needs a non-empty 'rows' array"};
            body += "#table(columns: (auto, 1fr), stroke: none, inset: 2pt,\n";
            for (const json& row : b["rows"]) {
                const std::string label = row.value("label", std::string{});
                if (label.empty())
                    return Error{i, "field_label_required", "every fields row needs a 'label'"};
                const std::string lname = emit_label(label);
                body += "  [#" + lname + "], ";

                if (row.contains("variable")) {
                    const std::string var = row["variable"].get<std::string>();
                    auto def = Variables::find(var);
                    if (!def)
                        return Error{i, "unknown_variable", "no such template variable: '" + var + "'"};
                    // Переменная приходит в input.json по своему же пути.
                    body += "[#d.at(" + detail::literal(Variables::input_path(*def)) + ", default: \"\")],\n";
                    properties[std::string(def->id)] = json{{"type", "string"}};
                } else {
                    const std::string field = row.value("field", std::string{});
                    if (!detail::is_valid_field(field))
                        return Error{i,
                                     "invalid_field_name",
                                     "field names are letters, digits and underscore, not starting with a digit"};
                    body += "[#d." + field + "],\n";
                    declare_field(field, label, row.value("label_kk", label), "string", row.value("required", true));
                }
            }
            body += ")\n#v(3mm)\n";

        } else if (type == BlockType::kTable) {
            if (!b.contains("columns") || !b["columns"].is_array() || b["columns"].empty())
                return Error{i, "table_columns_required", "a table block needs a non-empty 'columns' array"};
            const std::string items_field = b.value("field", std::string("items"));
            if (!detail::is_valid_field(items_field))
                return Error{i, "invalid_field_name", "the table's 'field' must be a valid field name"};

            std::vector<std::string> keys;
            std::string header_cells;
            std::string widths;
            for (const json& c : b["columns"]) {
                const std::string title = c.value("title", std::string{});
                const std::string key = c.value("key", std::string{});
                if (title.empty() || !detail::is_valid_field(key))
                    return Error{i, "table_column_invalid", "every column needs a 'title' and a valid 'key'"};
                const std::string lname = emit_label(title);
                header_cells += "[*#" + lname + "*], ";
                // 1fr ТОЛЬКО на колонке описания: на всех сразу таблица
                // расползается за поле, что гейт валит как выход за границу.
                widths += (key == "name" || key == "description") ? "1fr, " : "auto, ";
                keys.push_back(key);
            }
            body += "#table(columns: (" + widths + "),\n  " + header_cells + "\n";
            body += "  ..d." + items_field + ".map(it => (";
            for (std::size_t k = 0; k < keys.size(); ++k)
                body += (k ? ", " : "") + std::string("[#it.at(") + detail::literal(keys[k]) +
                        ", default: \"\")]");
            body += ")).flatten()\n)\n#v(3mm)\n";

            json item_props = json::object();
            for (const auto& k : keys)
                item_props[k] = json{{"type", "string"}};
            properties[items_field] =
                json{{"type", "array"}, {"items", json{{"type", "object"}, {"properties", item_props}}}};
            required.push_back(items_field);
            form_fields.push_back(json{{"field", items_field},
                                       {"label_ru", "Позиции"},
                                       {"label_kk", "Жолдар"},
                                       {"widget", "table"},
                                       {"columns", b["columns"]}});

        } else if (type == BlockType::kTotals) {
            // Денежные строки: пользователь вводит ЦЕЛОЕ в тиынах, а строку и
            // сумму прописью пишет сервер (P3 §3). Поэтому в форме — одно
            // числовое поле, а в шаблоне печатаются серверные строки.
            const std::string tiyn = b.value("field", std::string("total_tiyn"));
            if (!detail::is_valid_field(tiyn))
                return Error{i, "invalid_field_name", "the totals 'field' must be a valid field name"};
            const std::string label = b.value("label", std::string("Итого к оплате"));
            const std::string lname = emit_label(label);
            const std::string words_label = b.value("words_label", std::string("Сумма прописью"));
            const std::string wname = emit_label(words_label);

            body += "#align(right)[#" + lname + ": #d.total]\n";
            body += "#" + wname + ": #d.total_words\n#v(3mm)\n";

            properties[tiyn] = json{{"type", "integer"}, {"minimum", 0}};
            properties["total"] = json{{"type", "string"}};
            properties["total_words"] = json{{"type", "string"}};
            required.push_back(tiyn);
            required.push_back("total");
            required.push_back("total_words");
            form_fields.push_back(json{
                {"field", tiyn}, {"label_ru", label}, {"label_kk", b.value("label_kk", label)}, {"widget", "money"}});

        } else if (type == BlockType::kSignatures) {
            if (!b.contains("parties") || !b["parties"].is_array() || b["parties"].empty())
                return Error{i, "signature_parties_required", "a signatures block needs a non-empty 'parties' array"};
            // Линия подписи — box с рамкой СНИЗУ, никогда голая line(): нулевая
            // высота дала бы линию, проведённую сквозь текст, и это ровно тот
            // дефект, ради которого в гейте появился слой «линии сквозь текст».
            preamble += "#let sigrule = box(width: 1fr, height: 8mm, stroke: (bottom: 0.4pt))\n";
            body += "#v(6mm)\n#grid(columns: (1fr,) * " + std::to_string(b["parties"].size()) + ", gutter: 8mm,\n";
            for (const json& party : b["parties"]) {
                const std::string title = party.is_string() ? party.get<std::string>() : party.value("title", "");
                if (title.empty())
                    return Error{i, "signature_title_required", "every signature party needs a title"};
                const std::string lname = emit_label(title);
                body += "  [#" + lname + " #sigrule],\n";
            }
            body += ")\n";

        } else if (type == BlockType::kPagebreak) {
            body += "#pagebreak()\n";

        } else {
            return Error{i, "unknown_block_type", "no such block type: '" + type + "'"};
        }
    }

    if (labels.empty())
        return Error{0,
                     "no_static_labels",
                     "a template with no static label at all cannot be gated — the content layer would have nothing "
                     "to check"};

    std::string source;
    source += "// Собран из блоков конструктором (src/docgen/BlockCompiler.hpp).\n";
    source += "// Править этот текст руками бессмысленно: источник правды — блоки,\n";
    source += "// и следующая публикация соберёт его заново.\n";
    source += "#set page(paper: \"a4\", margin: 18mm)\n";
    source += "// overhang: Typst свешивает финальный дефис ЗА поле; hyphenate:\n";
    source += "// перенос РАЗРЫВАЕТ слово, и объявленная подпись перестаёт\n";
    source += "// находиться как непрерывный текст. Локально этот класс дефектов\n";
    source += "// не воспроизводится — метрики шрифтов различаются.\n";
    source += "#set text(font: \"Noto Sans\", size: 9.2pt, lang: \"ru\", overhang: false, hyphenate: false)\n";
    source += "// Токен со слешем (№ 12/2026, шт/кг) иначе рвётся по слешу.\n";
    source += "#show regex(\"\\\\S+/\\\\S+\"): it => box(it)\n";
    source += "#let d = json(\"input.json\")\n";
    source += preamble;
    source += "\n";
    source += body;

    std::string expected;
    for (const auto& l : labels)
        expected += l + "\n";
    expected += "margin 18mm\n";

    out.source = std::move(source);
    out.schema = json{{"type", "object"}, {"required", required}, {"properties", properties}};
    out.form = json{{"fields", form_fields}};
    out.expected = std::move(expected);
    return std::nullopt;
}

}  // namespace Docgen::Blocks
