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

/// Эндпоинт, который владеет шаблоном @p slug — тот, у которого есть
/// авторитетные данные для этой формы. Пустая строка, если такого нет
/// (первичка: её порождает сам POST /documents/generate; неизвестный слаг:
/// его не порождает никто). Нужен, чтобы 422 unsupported_template называл
/// каллеру конкретный маршрут, а не отправлял его гадать.
inline const char* owning_endpoint(const std::string& slug) {
    if (slug == "payslip")
        return "POST /api/v1/payroll-runs/{id}/payslips/{employee_id}/generate-document";
    if (slug == "fno_910" || slug == "fno_300")
        return "POST /api/v1/tax/filings";
    if (slug == "hr_order")
        return "POST /api/v1/hr-orders/{id}/generate-document";
    if (slug == "labor_contract")
        return "POST /api/v1/labor-contracts/{id}/generate-document";
    return "";
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
    /// НЕОБЯЗАТЕЛЬНЫЕ части итога: {путь целого, путь его строки}. От
    /// `components` отличаются двумя вещами. (1) Часть можно не присылать
    /// вовсе — тогда её строка не пишется и шаблон свою строку не печатает.
    /// (2) Суммы они не образуют: проверяется только «часть не больше
    /// итога». Слабее — потому что счёт и АВР печатают ровно две итоговые
    /// денежные строки, НДС и «Итого к оплате», а нетто-строки в шаблоне
    /// нет; выводить из двух чисел третье было бы догадкой о том, включён
    /// НДС в итог или нет.
    ///
    /// Строка части ВСЕГДА серверная, ровно как строка итога: до P3
    /// `vat_amount` приходил от клиента свободной строкой и печатался
    /// строкой ВЫШЕ выведенного итога, так что счёт мог заявлять «НДС:
    /// 999 999,00 ₸» над «Итого к оплате: 1 120,00 ₸» — тот же класс
    /// подделки, что закрыт для счёта-фактуры, просто в другом шаблоне.
    std::vector<std::pair<std::string, std::string>> optional_parts;
};

inline std::optional<DerivedAmount> derived_amount_for(const std::string& slug) {
    // Счёт и АВР печатают строку НДС над строкой итога (см.
    // templates/docs/invoice/v1/template.typ и тот же блок у avr) — поэтому у
    // них есть необязательная часть. Накладная НДС не печатает вовсе, и её
    // схема поля НДС не объявляет.
    if (slug == "invoice" || slug == "avr")
        return DerivedAmount{"total_tiyn", "total", "total_words", {}, {{"vat_tiyn", "vat_amount"}}};
    if (slug == "waybill")
        return DerivedAmount{"total_tiyn", "total", "total_words", {}, {}};
    if (slug == "tax_invoice")
        return DerivedAmount{"totals.with_vat_tiyn",
                             "totals.with_vat",
                             "total_words",
                             {{"totals.amount_tiyn", "totals.amount"}, {"totals.vat_tiyn", "totals.vat"}},
                             {}};
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
 * @details Четыре обязанности: (1) отвергнуть любое серверно-выводимое поле,
 *          присланное каллером; (2) прочитать итог и все его слагаемые как
 *          целые в допустимом диапазоне; (3) если разбивка объявлена —
 *          проверить, что слагаемые дают РОВНО итог; (4) необязательные
 *          части итога, если присланы, прочитать целыми и потребовать, что
 *          ни одна не больше итога. (3) и (4) вместе делают несходящуюся
 *          ИТОГОВУЮ строку невозможной: ни печатать оборот и НДС, не
 *          дающие в сумме напечатанный итог, ни печатать НДС больше самого
 *          итога больше нельзя. Позиции документа этой проверкой не
 *          покрыты (см. DerivedAmount).
 * @return false + заполненные @p error_field / @p error_code /
 *         @p error_message. Каллер обязан превратить это в 422 — все три
 *         случая (чужое поле, кривое целое, несходящаяся разбивка) суть
 *         семантически неверные значения, а не кривая форма запроса.
 */
/// Реквизиты продавца, которые сервер подставляет вместо клиентских.
/// Заполняется вызывающим из организации и её основного счёта — сам заголовок
/// в базу не ходит.
struct SellerRequisites {
    std::string name;        ///< organizations.name — есть всегда
    std::string identifier;  ///< organizations.bin — есть всегда
    std::string address;     ///< organizations.legal_address, может быть пуст
    std::string iik;         ///< основной счёт, если назначен
    std::string bank;
    std::string bik;
    std::string kbe;
    std::string vat_certificate;  ///< только счёт-фактура
};

/// Шаблоны, у которых продавец — это САМА организация, и потому он не может
/// приходить от клиента. `reconciliation` сюда НЕ входит намеренно: у него
/// стороны называются party_a/party_b, и какая из них «мы» — вопрос
/// вызывающего, а не свойство схемы. Догадка здесь напечатала бы чужие
/// реквизиты в акте сверки.
inline bool seller_is_the_organization(const std::string& slug) {
    return slug == "invoice" || slug == "avr" || slug == "waybill" || slug == "tax_invoice";
}

/**
 * @brief Записать реквизиты продавца от лица сервера.
 * @details Тот же принцип, что у сумм: поле, которое обязан заполнять сервер,
 *          от клиента не принимается вовсе. До этого `seller` приходил в теле
 *          целиком (`definitions.party` в схемах объявляет name, identifier,
 *          address, iik, bik, kbe), из-за чего бухгалтер перенабирал свои же
 *          банковские реквизиты в каждом документе, и два счёта одной
 *          организации могли разойтись в номере счёта.
 *
 *          Имя и БИН пишутся ВСЕГДА — они у организации есть по определению,
 *          и `seller` обязателен во всех четырёх схемах. Остальные поля
 *          пишутся, только если заполнены: организация, ещё не внёсшая адрес
 *          и счёт, продолжает выпускать документы, просто без этих строк.
 *          Это сознательный выбор в пользу работоспособности — жёсткая
 *          проверка «сначала заполните реквизиты» сломала бы выпуск
 *          документов всем, кто их пока не внёс.
 * @return false и заполненные поля ошибки, если клиент прислал `seller`.
 */
inline bool apply_seller_requisites(const std::string& slug,
                                    json& input,
                                    const SellerRequisites& seller,
                                    std::string& error_field,
                                    std::string& error_code,
                                    std::string& error_message) {
    if (!seller_is_the_organization(slug))
        return true;

    if (input.contains("seller")) {
        error_field = "input.seller";
        error_code = "not_allowed_override";
        error_message =
            "'seller' is filled in by the server from the organization's own requisites and may not be supplied "
            "by the client";
        return false;
    }

    json party = json::object();
    party["name"] = seller.name;
    party["identifier"] = seller.identifier;
    // Пустое поле НЕ пишется: схема объявляет его необязательным, а пустая
    // строка напечатала бы в документе пустую подпись вместо её отсутствия.
    if (!seller.address.empty())
        party["address"] = seller.address;
    if (!seller.iik.empty())
        party["iik"] = seller.iik;
    if (!seller.bank.empty())
        party["bank"] = seller.bank;
    if (!seller.bik.empty())
        party["bik"] = seller.bik;
    if (!seller.kbe.empty())
        party["kbe"] = seller.kbe;
    // Свидетельство по НДС объявлено ТОЛЬКО в схеме счёта-фактуры. Писать
    // его в остальные документы нельзя: их схемы поле не объявляют, и
    // валидация отвергла бы собственный ввод сервера.
    if (slug == "tax_invoice" && !seller.vat_certificate.empty())
        party["vat_certificate"] = seller.vat_certificate;
    input["seller"] = std::move(party);
    return true;
}

inline bool apply_derived_amount(const std::string& slug,
                                 json& input,
                                 std::string& error_field,
                                 std::string& error_code,
                                 std::string& error_message) {
    auto derived = derived_amount_for(slug);
    if (!derived)
        return true;

    // (1) Ни одна строка, которую пишет сервер, не принимается от каллера —
    // включая строки слагаемых и НЕОБЯЗАТЕЛЬНЫХ частей. Для последних это
    // проверяется безусловно, даже если целое не прислано: иначе `vat_amount`
    // без `vat_tiyn` проскочил бы как свободная строка — ровно та дыра,
    // которая пережила P3 в счёте и АВР.
    std::vector<std::string> server_written = {derived->amount_path, derived->words_path};
    for (const auto& c : derived->components)
        server_written.push_back(c.second);
    for (const auto& p : derived->optional_parts)
        server_written.push_back(p.second);
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

    // (4) Необязательные части: присланные читаются как целые и обязаны не
    // превосходить итог; неприсланные не порождают строки вовсе, и шаблон
    // свою условную строку не печатает.
    std::vector<std::pair<std::string, long long>> optional_values;
    optional_values.reserve(derived->optional_parts.size());
    for (const auto& p : derived->optional_parts) {
        if (at_path(input, p.first) == nullptr)
            continue;
        long long value = 0;
        if (!detail::read_tiyn(input, p.first, value, error_field, error_code, error_message))
            return false;
        if (value > total_tiyn) {
            error_field = "input." + p.first;
            error_code = "exceeds_total";
            error_message = "'" + p.first + "' (" + std::to_string(value) + " tiyn) is larger than " +
                            derived->tiyn_path + " (" + std::to_string(total_tiyn) +
                            " tiyn) — a part of the amount due cannot exceed the amount due";
            return false;
        }
        optional_values.emplace_back(p.second, value);
    }

    set_path(input, derived->amount_path, json(Money::format_tiyn_ru(total_tiyn)));
    set_path(input, derived->words_path, json(Money::to_words_ru(total_tiyn)));
    for (std::size_t i = 0; i < derived->components.size(); ++i)
        set_path(input, derived->components[i].second, json(Money::format_tiyn_ru(component_values[i])));
    for (const auto& v : optional_values)
        set_path(input, v.first, json(Money::format_tiyn_ru(v.second)));
    return true;
}

}  // namespace Docgen::InputPolicy
