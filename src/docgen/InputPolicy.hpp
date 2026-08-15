/**
 * @file InputPolicy.hpp
 * @brief Одна таблица на всю систему: что клиент вправе прислать в `input`
 *        документа и какие денежные поля сервер выводит сам.
 * @details До P3 эти правила были размазаны по четырём контроллерам как
 *          приватные функции *_allowed_extra_fields(), а у
 *          POST /documents/generate их не было вовсе. Собраны сюда, потому
 *          что правку документа (задача 9) обязан пропускать ТОТ ЖЕ
 *          allowlist, что и создание: input_snapshot — ровно то, что
 *          рендерит джоба, и приём его целиком заново открывает дыру
 *          подделки, закрытую в P2 (PDF декларации с балансом 1 тенге при
 *          правдивом XML той же записи).
 *
 *          Точка внедрения прописей: сервер кладёт их в JSON `input` ДО
 *          TemplateRegistry::validate(); шаблон остаётся с плейсхолдером
 *          {{ total_words }}. Считать пропись внутри .tex или переносить
 *          поле из схемы в шаблон запрещено — миграция на Typst будет это
 *          разбирать обратно.
 */

#pragma once

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "money/AmountInWords.hpp"
#include "money/MoneyFormat.hpp"

namespace Docgen::InputPolicy {

using json = nlohmann::json;

/// Слаги, которые POST /api/v1/documents/generate имеет право создавать.
/// Ровно те значения, которые migrations/010_documents.sql перечисляет в
/// CHECK на doc_type для первички — слаг идёт в doc_type дословно, поэтому
/// любой другой слаг нарушает documents_doc_type_check и до P3 давал 500.
inline const std::vector<std::string>& generate_slugs() {
    static const std::vector<std::string> kSlugs = {"invoice", "avr", "waybill", "tax_invoice", "reconciliation"};
    return kSlugs;
}

/// true — весь `input` авторский (первичка: за ней в БД ничего не стоит).
/// false — форму строит контроллер из авторитетных строк, и editable_fields()
/// исчерпывающе перечисляет, что клиенту дозволено добавить.
inline bool input_is_caller_authored(const std::string& slug) {
    const auto& s = generate_slugs();
    return std::find(s.begin(), s.end(), slug) != s.end();
}

/// Денежные поля, которые сервер форматирует сам из целых чисел тиын. Пути
/// точечные и не глубже одного уровня вложенности.
struct DerivedAmount {
    std::string tiyn_path;  ///< итоговое целое, которое ОБЯЗАН прислать клиент
    std::string amount_path;  ///< сюда сервер пишет Money::format_tiyn_ru(итог)
    std::string words_path;   ///< сюда сервер пишет Money::to_words_ru(итог)
    /// Слагаемые итога: {путь целого, путь его строки}. Пустой список —
    /// разбивки нет. Непустой означает ДВЕ вещи: сервер форматирует каждое
    /// слагаемое сам И проверяет, что сумма слагаемых равна ровно итогу.
    /// Счёт-фактура, где напечатанные оборот и НДС не сходятся с
    /// напечатанной итоговой суммой, — та же поверхность подделки, что
    /// убрана из ФНО 300.00.
    ///
    /// Область проверки ровно одна: ИТОГОВАЯ строка документа. Строки
    /// позиций (items[].amount и соседи) остаются свободным текстом и с
    /// итогом не сверяются — счёт-фактура, чьи позиции не дают заявленный
    /// оборот, всё ещё выпускается. Сверка позиций — отдельная работа, вне
    /// области P3.
    std::vector<std::pair<std::string, std::string>> components;
};

inline std::optional<DerivedAmount> derived_amount_for(const std::string& slug) {
    if (slug == "invoice" || slug == "avr" || slug == "waybill")
        return DerivedAmount{"total_tiyn", "total", "total_words", {}};
    if (slug == "tax_invoice")
        return DerivedAmount{"totals.with_vat_tiyn",
                             "totals.with_vat",
                             "total_words",
                             {{"totals.amount_tiyn", "totals.amount"}, {"totals.vat_tiyn", "totals.vat"}}};
    return std::nullopt;  // reconciliation и все серверные формы
}

/// Единственные ключи, которые каллер вправе прислать для серверно
/// строящейся формы. Пустой вектор = «ни одного». Точечный путь адресует
/// один лист ("employer.director" разрешает только его, но не соседние
/// authoritative employer.name/employer.bin).
inline const std::vector<std::string>& editable_fields(const std::string& slug) {
    static const std::vector<std::string> kNone = {};
    static const std::vector<std::string> kFnoSignatories = {"director", "accountant"};
    static const std::vector<std::string> kHrOrder = {"director", "reason", "details"};
    static const std::vector<std::string> kLaborContract = {
        "work_schedule", "probation_months", "employer.director", "employer.address", "employee.address"};
    if (slug == "fno_910" || slug == "fno_300")
        return kFnoSignatories;
    if (slug == "hr_order")
        return kHrOrder;
    if (slug == "labor_contract")
        return kLaborContract;
    return kNone;  // payslip, вся первичка и любой неизвестный слаг
}

/// Прочитать точечный путь. nullptr, если любой сегмент отсутствует или
/// промежуточный узел не объект.
inline const json* at_path(const json& obj, const std::string& path) {
    const json* node = &obj;
    std::size_t start = 0;
    while (start <= path.size()) {
        const std::size_t dot = path.find('.', start);
        const std::string key = path.substr(start, dot == std::string::npos ? std::string::npos : dot - start);
        if (!node->is_object() || !node->contains(key))
            return nullptr;
        node = &(*node)[key];
        if (dot == std::string::npos)
            return node;
        start = dot + 1;
    }
    return nullptr;
}

/// Записать по точечному пути, создавая промежуточные объекты.
inline void set_path(json& obj, const std::string& path, json value) {
    const std::size_t dot = path.find('.');
    if (dot == std::string::npos) {
        obj[path] = std::move(value);
        return;
    }
    const std::string head = path.substr(0, dot);
    if (!obj.contains(head) || !obj[head].is_object())
        obj[head] = json::object();
    set_path(obj[head], path.substr(dot + 1), std::move(value));
}

namespace detail {

/// Прочитать одно целое поле тиын по точечному пути. false + заполненная
/// тройка ошибки, если поле отсутствует, не целое или вне диапазона.
inline bool read_tiyn(const json& input,
                      const std::string& path,
                      long long& out,
                      std::string& error_field,
                      std::string& error_code,
                      std::string& error_message) {
    const json* node = at_path(input, path);
    if (node == nullptr) {
        error_field = "input." + path;
        error_code = "missing";
        error_message = "'" + path + "' is required — money is carried as an integer number of tiyn";
        return false;
    }
    if (!node->is_number_integer()) {
        error_field = "input." + path;
        error_code = "not_integer";
        error_message = "'" + path + "' must be an integer number of tiyn";
        return false;
    }
    const long long value = node->get<long long>();
    if (value < 0 || value > Money::kMaxTiyn) {
        error_field = "input." + path;
        error_code = "out_of_range";
        error_message = "'" + path + "' must be between 0 and " + std::to_string(Money::kMaxTiyn) + " tiyn inclusive";
        return false;
    }
    out = value;
    return true;
}

}  // namespace detail

/**
 * @brief Проверить целые денежные поля и записать в @p input их строковые
 *        представления и пропись итога. Для слага без деривации — no-op с
 *        результатом true.
 * @details Три обязанности: (1) отвергнуть любое серверно-выводимое поле,
 *          присланное каллером; (2) прочитать итог и все его слагаемые как
 *          целые в допустимом диапазоне; (3) если разбивка объявлена —
 *          проверить, что слагаемые дают РОВНО итог. Последнее делает
 *          несходящуюся ИТОГОВУЮ строку невозможной: печатать оборот и
 *          НДС, не дающие в сумме напечатанный итог, больше нельзя.
 *          Позиции документа этой проверкой не покрыты (см. DerivedAmount).
 * @return false + заполненные @p error_field / @p error_code /
 *         @p error_message. Каллер обязан превратить это в 422 — все три
 *         случая (чужое поле, кривое целое, несходящаяся разбивка) суть
 *         семантически неверные значения, а не кривая форма запроса.
 */
inline bool apply_derived_amount(const std::string& slug,
                                 json& input,
                                 std::string& error_field,
                                 std::string& error_code,
                                 std::string& error_message) {
    auto derived = derived_amount_for(slug);
    if (!derived)
        return true;

    // (1) Ни одна строка, которую пишет сервер, не принимается от каллера —
    // включая строки слагаемых.
    std::vector<std::string> server_written = {derived->amount_path, derived->words_path};
    for (const auto& c : derived->components)
        server_written.push_back(c.second);
    for (const auto& path : server_written) {
        if (at_path(input, path) != nullptr) {
            error_field = "input." + path;
            error_code = "not_allowed_override";
            error_message = "'" + path + "' is formatted by the server from the integer tiyn fields and may not be " +
                            "supplied by the client";
            return false;
        }
    }

    // (2) Итог и слагаемые — целые в допустимом диапазоне.
    long long total_tiyn = 0;
    if (!detail::read_tiyn(input, derived->tiyn_path, total_tiyn, error_field, error_code, error_message))
        return false;
    std::vector<long long> component_values;
    component_values.reserve(derived->components.size());
    for (const auto& c : derived->components) {
        long long value = 0;
        if (!detail::read_tiyn(input, c.first, value, error_field, error_code, error_message))
            return false;
        component_values.push_back(value);
    }

    // (3) Разбивка обязана сходиться точно. Сложение безопасно: каждое
    // слагаемое уже ограничено kMaxTiyn, а их не больше горстки, так что
    // до переполнения long long далеко.
    if (!derived->components.empty()) {
        long long sum = 0;
        for (const long long v : component_values)
            sum += v;
        if (sum != total_tiyn) {
            std::string parts;
            for (std::size_t i = 0; i < derived->components.size(); ++i) {
                if (i > 0)
                    parts += " + ";
                parts += derived->components[i].first + " (" + std::to_string(component_values[i]) + ")";
            }
            error_field = "input." + derived->tiyn_path;
            error_code = "inconsistent_total";
            error_message = parts + " = " + std::to_string(sum) + " tiyn, which does not equal " + derived->tiyn_path +
                            " (" + std::to_string(total_tiyn) + ")";
            return false;
        }
    }

    set_path(input, derived->amount_path, json(Money::format_tiyn_ru(total_tiyn)));
    set_path(input, derived->words_path, json(Money::to_words_ru(total_tiyn)));
    for (std::size_t i = 0; i < derived->components.size(); ++i)
        set_path(input, derived->components[i].second, json(Money::format_tiyn_ru(component_values[i])));
    return true;
}

}  // namespace Docgen::InputPolicy
