/**
 * @file Variables.hpp
 * @brief Каталог заранее заготовленных переменных шаблона (спека §7).
 *
 * ПОЧЕМУ КАТАЛОГ, А НЕ СВОБОДНЫЙ ТЕКСТ. Пользователь просил «набор переменных
 * типа %НДС, оклад сотрудника, ФИО директора, расчётные счета». Соблазн —
 * разрешить произвольное имя и подставлять что найдётся. Так делать нельзя:
 * опечатка в имени тогда молча печатает пустоту, а не отвергается, и документ
 * выходит без реквизита, о котором никто не узнает. Здесь набор ЗАКРЫТ и
 * объявлен кодом: неизвестный идентификатор — ошибка сборки шаблона.
 *
 * СТАВКА НДС берётся из налогового движка НА ДАТУ ДОКУМЕНТА, а не константой в
 * тексте: спека §7.1 требует, чтобы ставки были данными, а не кодом, и шаблон
 * здесь не исключение. Записать «16%» в шаблон значит получить неверный
 * документ в день, когда ставка изменится.
 *
 * Заголовок НЕ ходит в базу. Он объявляет, ЧТО существует и как это
 * называется по-русски и по-казахски; значения подставляет вызывающий, у
 * которого уже есть загруженные организация, сотрудник и контрагент. Так
 * каталог остаётся проверяемым без Postgres.
 */

#pragma once

#include <array>
#include <optional>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace Docgen::Variables {

using json = nlohmann::json;

/// Откуда берётся значение. Определяет, какие данные вызывающий обязан
/// загрузить, прежде чем собирать документ по шаблону, использующему
/// переменную.
enum class Source {
    kOrganization,  ///< организация и её реквизиты
    kBankAccount,   ///< ОСНОВНОЙ расчётный счёт организации
    kTaxRate,       ///< налоговый движок, на дату документа
    kEmployee,      ///< сотрудник (кадровые и зарплатные документы)
    kCounterparty,  ///< контрагент документа
    kDocument,      ///< поле самого документа, приходит из формы
};

/// Тип значения. `kMoneyTiyn` существует отдельно от `kText` не для красоты:
/// денежная переменная обязана прийти ЦЕЛЫМ в тиынах, а печатаемую строку и
/// сумму прописью выводит сервер — иначе цифра и текст в одном документе
/// могут разойтись (P3 §3).
enum class Kind { kText, kMoneyTiyn, kDate, kPercent };

struct Definition {
    std::string_view id;
    std::string_view label_ru;
    std::string_view label_kk;
    Source source;
    Kind kind;
};

/// Закрытый каталог. Порядок — тот, в котором переменные показываются в
/// конструкторе: сначала своё, потом контрагент, потом поля документа.
inline constexpr std::array kCatalogue = {
    Definition{"org.name", "Название организации", "Ұйым атауы", Source::kOrganization, Kind::kText},
    Definition{"org.bin", "БИН", "БСН", Source::kOrganization, Kind::kText},
    Definition{"org.address", "Юридический адрес", "Заңды мекенжайы", Source::kOrganization, Kind::kText},
    Definition{"org.director_name", "ФИО подписанта", "Қол қоюшының Т.А.Ә.", Source::kOrganization, Kind::kText},
    Definition{
        "org.director_position", "Должность подписанта", "Қол қоюшының лауазымы", Source::kOrganization, Kind::kText},
    Definition{"org.vat_certificate", "Свидетельство по НДС", "ҚҚС бойынша куәлік", Source::kOrganization, Kind::kText},
    Definition{"org.iik", "ИИК (основной счёт)", "ЖСК (негізгі шот)", Source::kBankAccount, Kind::kText},
    Definition{"org.bank_name", "Банк", "Банк", Source::kBankAccount, Kind::kText},
    Definition{"org.bik", "БИК", "БСК", Source::kBankAccount, Kind::kText},
    Definition{"org.kbe", "КБе", "БСК коды", Source::kBankAccount, Kind::kText},
    Definition{"tax.vat_rate", "Ставка НДС", "ҚҚС мөлшерлемесі", Source::kTaxRate, Kind::kPercent},
    Definition{"employee.full_name", "ФИО сотрудника", "Қызметкердің Т.А.Ә.", Source::kEmployee, Kind::kText},
    Definition{"employee.position", "Должность сотрудника", "Қызметкердің лауазымы", Source::kEmployee, Kind::kText},
    Definition{
        "employee.salary_tiyn", "Оклад сотрудника", "Қызметкердің жалақысы", Source::kEmployee, Kind::kMoneyTiyn},
    Definition{"counterparty.name", "Контрагент", "Контрагент", Source::kCounterparty, Kind::kText},
    Definition{
        "counterparty.identifier", "БИН/ИИН контрагента", "Контрагенттің БСН/ЖСН", Source::kCounterparty, Kind::kText},
    Definition{
        "counterparty.address", "Адрес контрагента", "Контрагенттің мекенжайы", Source::kCounterparty, Kind::kText},
    Definition{"doc.number", "Номер документа", "Құжат нөмірі", Source::kDocument, Kind::kText},
    Definition{"doc.date", "Дата документа", "Құжат күні", Source::kDocument, Kind::kDate},
};

/// Определение по идентификатору, либо пусто, если такой переменной нет.
/// Пустой ответ — это ОТКАЗ собрать шаблон, а не повод подставить пустоту.
inline std::optional<Definition> find(std::string_view id) {
    for (const auto& d : kCatalogue) {
        if (d.id == id)
            return d;
    }
    return std::nullopt;
}

/// Путь в `input.json`, по которому шаблон читает переменную. Совпадает с
/// идентификатором: одно имя и в конструкторе, и в схеме, и в собранном
/// шаблоне — иначе их расхождение пришлось бы держать в голове.
inline std::string input_path(const Definition& d) {
    return std::string(d.id);
}

/// Каталог в виде JSON для интерфейса конструктора: идентификатор, обе
/// подписи, тип. `source` наружу не отдаётся — арендатору незачем знать, из
/// какой таблицы значение приходит, а нам незачем обещать это в API.
inline json to_json() {
    json out = json::array();
    for (const auto& d : kCatalogue) {
        out.push_back(json{
            {"id", std::string(d.id)},
            {"label_ru", std::string(d.label_ru)},
            {"label_kk", std::string(d.label_kk)},
            {"kind",
             d.kind == Kind::kMoneyTiyn ? "money"
             : d.kind == Kind::kDate    ? "date"
             : d.kind == Kind::kPercent ? "percent"
                                        : "text"},
        });
    }
    return out;
}

}  // namespace Docgen::Variables
