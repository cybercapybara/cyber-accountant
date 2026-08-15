/**
 * @file test_journal_cascade.cpp
 * @brief Каскад проверяется ТЕСТОМ, а не рассуждением. Организация с
 *        проведённой проводкой, её сторно, документами, ФНО-филингом и
 *        кадровым приказом одновременно: reverses_entry_id — самоссылка
 *        NO ACTION, tax_filings.document_id и hr_orders.document_id — тоже
 *        NO ACTION (первый DEFERRABLE, то есть срабатывает на COMMIT).
 *
 *        Проверяется вырез из migrations/020_journal_cascade_carveout.sql
 *        в редакции migrations/021_journal_carveout_schema_qualified.sql:
 *        два ранее заблокированных пути (DELETE FROM organizations и
 *        DELETE FROM users для автора проведённой записи) проходят, а
 *        прямые DELETE/UPDATE проведённой записи по-прежнему отвергаются.
 *
 *        Ничего здесь не идёт через репозитории, кроме создания тенанта и
 *        пользователя: инварианты живут в БД и обязаны держаться независимо
 *        от того, какой прикладной код пишет в эти таблицы (та же идиома,
 *        что в test_journal_schema.cpp).
 *
 *        Fix round 1 (ревью на живой PostgreSQL 16.15) добавил три теста на
 *        подмену имён через pg_temp: обращения к organizations/users/
 *        journal_lines внутри SECURITY INVOKER-функции были без схемы, а
 *        pg_temp просматривается при разрешении имён ОТНОШЕНИЙ первой и
 *        право создавать временные таблицы есть у PUBLIC — то есть обычная
 *        роль вешала CREATE TEMP TABLE organizations и правила/удаляла
 *        проведённые проводки. Лечится квалификацией public.* в
 *        migrations/021_journal_carveout_schema_qualified.sql; все три теста
 *        проверены на том, что БЕЗ 021 они падают.
 *
 *        Плюс DeletingTheAuthorChangesTheAuthorColumnAndNothingElse: ветка
 *        удаления автора сравнивает строку целиком (to_jsonb), а не список
 *        колонок — в 020 список из пяти имён оставлял id, created_at и
 *        created_by_run_id незакреплёнными, и created_at проведённой записи
 *        переписывался на 1970 год.
 *
 *        NB: теста «wipe_org_data() не бросает» здесь НЕТ намеренно —
 *        wipe_org_data() TRUNCATE'ит journal_entries ДО удаления
 *        организаций, а TRUNCATE не запускает построчные триггеры вовсе,
 *        так что такой тест проходил бы и БЕЗ выреза, то есть не мог бы
 *        упасть ни при какой реализации. Каскад доказывается только
 *        настоящим DELETE FROM organizations по строкам, на которые
 *        проведённая проводка реально ссылается.
 */

#include <optional>
#include <pqxx/pqxx>
#include <stdexcept>
#include <string>

#include <gtest/gtest.h>
#include <spdlog/spdlog.h>

#include "database/Database.hpp"
#include "repositories/RoleRepository.hpp"
#include "repositories/UserRepository.hpp"
#include "tenancy/OrganizationRepository.hpp"
#include "test_helpers.hpp"

namespace {

/// Заглушает лог на время ожидаемо падающих транзакций: Database.hpp пишет
/// ERROR на КАЖДУЮ непрошедшую транзакцию, а в этом файле их больше десятка
/// — без этого вывод CI тонет в сообщениях об ошибках, которых мы как раз и
/// добиваемся. Уровень восстанавливается в деструкторе, в том числе когда
/// EXPECT_THROW уходит по исключению.
class QuietLogs {
public:
    QuietLogs() : previous_(spdlog::default_logger()->level()) {
        spdlog::default_logger()->set_level(spdlog::level::critical);
    }
    ~QuietLogs() { spdlog::default_logger()->set_level(previous_); }
    QuietLogs(const QuietLogs&) = delete;
    QuietLogs& operator=(const QuietLogs&) = delete;
    QuietLogs(QuietLogs&&) = delete;
    QuietLogs& operator=(QuietLogs&&) = delete;

private:
    spdlog::level::level_enum previous_;
};

class JournalCascadeTest : public TestHelpers::CoreBackedTest {
protected:
    void SetUp() override {
        TestHelpers::CoreBackedTest::SetUp();
        if (::testing::Test::IsSkipped())
            return;
        // Порядок важен: сначала org-данные (wipe_org_data() TRUNCATE'ит
        // журнал, обходя триггер неизменяемости), потом пользователи —
        // TRUNCATE users CASCADE иначе утащил бы journal_entries сам.
        TestHelpers::wipe_org_data();
        TestHelpers::truncate_users();
    }

    /// Счётчик уникальности BIN/ИИН/email внутри одного бинарника: фикстуры
    /// чистятся в SetUp, но два seed'а в одном тесте не должны совпасть.
    int seq_ = 0;

    std::string next_suffix() { return std::to_string(++seq_); }

    /// Ровно 12 цифр (BIN и ИИН объявлены CHAR(12)): префикс, нули, номер.
    static std::string twelve_digits(const std::string& prefix, const std::string& tail) {
        return prefix + std::string(12 - prefix.size() - tail.size(), '0') + tail;
    }

    /// Confirmed "User"-role user; та же идиома, что seed_user() в
    /// test_journal_service.cpp — created_by_user_id это FK на users(id).
    std::string seed_user(const std::string& email) {
        Repositories::RoleRepository roles;
        Repositories::UserRepository users;
        auto role = roles.find_by_name("User");
        if (!role) {
            ADD_FAILURE() << "role 'User' missing — seed migration?";
            throw std::runtime_error("seed role missing: User");
        }
        return users
            .create(email,
                    std::string("$argon2id$placeholder"),
                    std::nullopt,
                    std::nullopt,
                    role->id,
                    /*confirmed=*/true)
            .id;
    }

    /// Организация со ВСЕМИ связями сразу: проведённая проводка + её сторно
    /// + документ на проводке + ФНО-филинг на документе + кадровый приказ на
    /// другом документе. Возвращает org_id.
    std::string seed_fully_wired_org() {
        const std::string suffix = next_suffix();
        const std::string user_id = seed_user("cascade" + suffix + "@example.com");

        Tenancy::OrganizationRepository orgs;
        const std::string bin = twelve_digits("1112600", suffix);
        const std::string org_id = orgs.create(bin, "Cascade Test Org " + suffix, "snr_simplified", false).id;

        std::string first_id;
        std::string storno_id;
        std::string doc_id;
        std::string hr_doc_id;

        // Первая проводка: draft -> posted (сбалансированная, две строки).
        Database::get().execute_write([&](auto& txn) {
            first_id = txn.exec_params(
                              "INSERT INTO journal_entries (org_id, entry_date, description, created_by_user_id) "
                              "VALUES ($1, '2026-01-10', 'Проведённая проводка', $2) RETURNING id",
                              org_id,
                              user_id)
                           .at(0)
                           .at(0)
                           .template as<std::string>();
            txn.exec_params(
                "INSERT INTO journal_lines (org_id, entry_id, account_code, side, amount) "
                "VALUES ($1, $2, '1030', 'debit', '100.00'), ($1, $2, '6010', 'credit', '100.00')",
                org_id,
                first_id);
            return 0;
        });
        Database::get().execute_write([&](auto& txn) {
            txn.exec_params("UPDATE journal_entries SET status = 'posted' WHERE id = $1", first_id);
            return 0;
        });

        // Сторно: НОВАЯ проводка со ссылкой reverses_entry_id (самоссылка
        // NO ACTION — именно она и могла бы заблокировать каскад).
        Database::get().execute_write([&](auto& txn) {
            storno_id = txn.exec_params(
                               "INSERT INTO journal_entries (org_id, entry_date, description, reverses_entry_id, "
                               "created_by_user_id) "
                               "VALUES ($1, '2026-01-11', 'Сторно', $2, $3) RETURNING id",
                               org_id,
                               first_id,
                               user_id)
                            .at(0)
                            .at(0)
                            .template as<std::string>();
            txn.exec_params(
                "INSERT INTO journal_lines (org_id, entry_id, account_code, side, amount) "
                "VALUES ($1, $2, '6010', 'debit', '100.00'), ($1, $2, '1030', 'credit', '100.00')",
                org_id,
                storno_id);
            return 0;
        });
        Database::get().execute_write([&](auto& txn) {
            txn.exec_params("UPDATE journal_entries SET status = 'posted' WHERE id = $1", storno_id);
            return 0;
        });
        Database::get().execute_write([&](auto& txn) {
            txn.exec_params("UPDATE journal_entries SET status = 'reversed' WHERE id = $1", first_id);
            return 0;
        });

        // Документ на проведённой проводке + его версия (current_version_id —
        // циклический DEFERRABLE FK) + связь document_entries.
        Database::get().execute_write([&](auto& txn) {
            doc_id = txn.exec_params(
                            "INSERT INTO documents (org_id, doc_type, source, status) "
                            "VALUES ($1, 'invoice', 'generated', 'final') RETURNING id",
                            org_id)
                         .at(0)
                         .at(0)
                         .template as<std::string>();
            auto version_id = txn.exec_params(
                                     "INSERT INTO document_versions (org_id, document_id, version_no, "
                                     "created_by_user_id) VALUES ($1, $2, 1, $3) RETURNING id",
                                     org_id,
                                     doc_id,
                                     user_id)
                                  .at(0)
                                  .at(0)
                                  .template as<std::string>();
            txn.exec_params("UPDATE documents SET current_version_id = $1 WHERE id = $2", version_id, doc_id);
            txn.exec_params("INSERT INTO document_entries (org_id, document_id, entry_id) VALUES ($1, $2, $3)",
                            org_id,
                            doc_id,
                            first_id);
            return 0;
        });

        // ФНО-филинг на том же документе: tax_filings.document_id — NO ACTION
        // и DEFERRABLE INITIALLY DEFERRED, то есть проверяется на COMMIT.
        Database::get().execute_write([&](auto& txn) {
            auto calc_id = txn.exec_params(
                                  "INSERT INTO tax_calculations (org_id, kind, period_from, period_to, "
                                  "input_snapshot, result_snapshot, total_tiyn) "
                                  "VALUES ($1, 'snr_simplified', '2026-01-01', '2026-06-30', '{}', '{}', 1000) "
                                  "RETURNING id",
                                  org_id)
                               .at(0)
                               .at(0)
                               .template as<std::string>();
            txn.exec_params(
                "INSERT INTO tax_filings (org_id, kind, period_from, period_to, calculation_id, document_id) "
                "VALUES ($1, '910.00', '2026-01-01', '2026-06-30', $2, $3)",
                org_id,
                calc_id,
                doc_id);
            return 0;
        });

        // Кадровый приказ на ДРУГОМ документе: hr_orders.document_id — тоже
        // NO ACTION, но НЕ deferrable (проверяется в конце оператора).
        Database::get().execute_write([&](auto& txn) {
            hr_doc_id = txn.exec_params(
                               "INSERT INTO documents (org_id, doc_type, source, status) "
                               "VALUES ($1, 'hr', 'generated', 'final') RETURNING id",
                               org_id)
                            .at(0)
                            .at(0)
                            .template as<std::string>();
            auto employee_id = txn.exec_params(
                                      "INSERT INTO employees (org_id, iin, last_name, first_name, position, "
                                      "salary_tiyn, hired_on) "
                                      "VALUES ($1, $2, 'Иванов', 'Иван', 'Бухгалтер', 30000000, '2026-01-05') "
                                      "RETURNING id",
                                      org_id,
                                      twelve_digits("1111111", suffix))
                                   .at(0)
                                   .at(0)
                                   .template as<std::string>();
            txn.exec_params(
                "INSERT INTO hr_orders (org_id, employee_id, kind, number, issued_on, effective_from, document_id) "
                "VALUES ($1, $2, 'hire', $3, '2026-01-05', '2026-01-05', $4)",
                org_id,
                employee_id,
                suffix,
                hr_doc_id);
            return 0;
        });

        return org_id;
    }

    /// Единственная всё ещё проведённая запись организации — сторно (первая
    /// проводка после сторнирования лежит в статусе reversed).
    static std::string posted_entry_of(const std::string& org_id) {
        return Database::get().execute_read([&](auto& txn) {
            return txn
                .exec_params("SELECT id FROM journal_entries WHERE org_id = $1 AND status = 'posted' LIMIT 1", org_id)
                .at(0)
                .at(0)
                .template as<std::string>();
        });
    }

    static std::string author_of_the_posted_entry(const std::string& org_id) {
        return Database::get().execute_read([&](auto& txn) {
            return txn
                .exec_params(
                    "SELECT created_by_user_id FROM journal_entries "
                    "WHERE org_id = $1 AND status = 'posted' AND created_by_user_id IS NOT NULL LIMIT 1",
                    org_id)
                .at(0)
                .at(0)
                .template as<std::string>();
        });
    }
};

TEST_F(JournalCascadeTest, DeletingAnOrganizationCascadesThroughPostedEntries) {
    const std::string org_id = seed_fully_wired_org();
    EXPECT_NO_THROW(Database::get().execute_write([&](auto& txn) {
        txn.exec_params("DELETE FROM organizations WHERE id = $1", org_id);
        return 0;
    }));
    const auto left = Database::get().execute_read([&](auto& txn) {
        auto r = txn.exec_params("SELECT COUNT(*) FROM journal_entries WHERE org_id = $1", org_id);
        return r.at(0).at(0).template as<long>();
    });
    EXPECT_EQ(left, 0);
    // Каскад обязан дойти до КОНЦА, а не остановиться на журнале: если бы
    // reverses_entry_id / tax_filings.document_id / hr_orders.document_id
    // (все три NO ACTION) блокировали удаление, транзакция откатилась бы
    // целиком и организация осталась бы на месте.
    const auto org_rows = Database::get().execute_read([&](auto& txn) {
        auto r = txn.exec_params("SELECT COUNT(*) FROM organizations WHERE id = $1", org_id);
        return r.at(0).at(0).template as<long>();
    });
    EXPECT_EQ(org_rows, 0);
    const auto related = Database::get().execute_read([&](auto& txn) {
        auto r = txn.exec_params(
            "SELECT (SELECT COUNT(*) FROM journal_lines WHERE org_id = $1) "
            "     + (SELECT COUNT(*) FROM documents WHERE org_id = $1) "
            "     + (SELECT COUNT(*) FROM tax_filings WHERE org_id = $1) "
            "     + (SELECT COUNT(*) FROM hr_orders WHERE org_id = $1)",
            org_id);
        return r.at(0).at(0).template as<long>();
    });
    EXPECT_EQ(related, 0);
}

TEST_F(JournalCascadeTest, DeletingAUserWhoPostedAnEntrySetsTheAuthorToNull) {
    const std::string org_id = seed_fully_wired_org();
    const std::string user_id = author_of_the_posted_entry(org_id);
    EXPECT_NO_THROW(Database::get().execute_write([&](auto& txn) {
        txn.exec_params("DELETE FROM users WHERE id = $1", user_id);
        return 0;
    }));
    const auto orphaned = Database::get().execute_read([&](auto& txn) {
        auto r = txn.exec_params(
            "SELECT COUNT(*) FROM journal_entries WHERE org_id = $1 AND created_by_user_id IS NULL", org_id);
        return r.at(0).at(0).template as<long>();
    });
    EXPECT_GT(orphaned, 0);
    // Сама проводка на месте и всё ещё posted — удаление автора не трогает
    // содержание записи.
    const auto still_posted = Database::get().execute_read([&](auto& txn) {
        auto r =
            txn.exec_params("SELECT COUNT(*) FROM journal_entries WHERE org_id = $1 AND status = 'posted'", org_id);
        return r.at(0).at(0).template as<long>();
    });
    EXPECT_GT(still_posted, 0);
}

/// Ветка «автор удалён» на настоящем каскаде: меняется РОВНО одна колонка.
/// Сравнение идёт по всей строке (to_jsonb), поэтому тест поймает и добавление
/// нового поля в каскад, и возврат к ручному списку колонок — под 020 список
/// из пяти имён оставлял id, created_at и created_by_run_id незакреплёнными.
TEST_F(JournalCascadeTest, DeletingTheAuthorChangesTheAuthorColumnAndNothingElse) {
    const std::string org_id = seed_fully_wired_org();
    const std::string entry_id = posted_entry_of(org_id);
    const std::string user_id = author_of_the_posted_entry(org_id);

    const std::string before = Database::get().execute_read([&](auto& txn) {
        return txn.exec_params("SELECT to_jsonb(j) - 'updated_at' FROM journal_entries j WHERE id = $1", entry_id)
            .at(0)
            .at(0)
            .template as<std::string>();
    });

    EXPECT_NO_THROW(Database::get().execute_write([&](auto& txn) {
        txn.exec_params("DELETE FROM users WHERE id = $1", user_id);
        return 0;
    }));

    const bool only_the_author_changed = Database::get().execute_read([&](auto& txn) {
        return txn
            .exec_params(
                "SELECT (to_jsonb(j) - 'updated_at') "
                "     = (jsonb_set($2::jsonb, '{created_by_user_id}', 'null'::jsonb) - 'updated_at') "
                "  FROM journal_entries j WHERE id = $1",
                entry_id,
                before)
            .at(0)
            .at(0)
            .template as<bool>();
    });
    EXPECT_TRUE(only_the_author_changed)
        << "каскад ON DELETE SET NULL обязан менять только created_by_user_id; строка до удаления: " << before;
}

/// pg_temp-подмена имени organizations. БЕЗ квалификации public.* обе
/// операции ниже проходили: пустая временная таблица делает NOT EXISTS
/// истинным, и вырез считает организацию удалённой. Право CREATE TEMP есть
/// у PUBLIC, так что это доступно любой роли.
TEST_F(JournalCascadeTest, TempTableNamedOrganizationsCannotDisableImmutability) {
    const std::string org_id = seed_fully_wired_org();
    const std::string entry_id = posted_entry_of(org_id);
    QuietLogs quiet;
    // ON COMMIT DROP: если вырез когда-нибудь снова пропустит эту операцию,
    // транзакция закоммитится — и временная таблица не должна пережить её на
    // пуловом соединении, иначе отравит все последующие тесты.
    EXPECT_THROW(Database::get().execute_write([&](auto& txn) {
        txn.exec("CREATE TEMP TABLE organizations (id uuid) ON COMMIT DROP");
        txn.exec_params("UPDATE journal_entries SET description = 'подделка', status = 'draft' WHERE id = $1",
                        entry_id);
        return 0;
    }),
                 pqxx::sql_error);
    EXPECT_THROW(Database::get().execute_write([&](auto& txn) {
        txn.exec("CREATE TEMP TABLE organizations (id uuid) ON COMMIT DROP");
        txn.exec_params("DELETE FROM journal_entries WHERE id = $1", entry_id);
        return 0;
    }),
                 pqxx::sql_error);
}

/// pg_temp-подмена имени users открывала ветку удаления автора при живом
/// авторе — через неё переписывался created_at проведённой записи.
TEST_F(JournalCascadeTest, TempTableNamedUsersCannotOpenTheAuthorBranch) {
    const std::string org_id = seed_fully_wired_org();
    const std::string entry_id = posted_entry_of(org_id);
    QuietLogs quiet;
    EXPECT_THROW(Database::get().execute_write([&](auto& txn) {
        txn.exec("CREATE TEMP TABLE users (id uuid) ON COMMIT DROP");
        txn.exec_params("UPDATE journal_entries SET created_by_user_id = NULL, created_at = '1970-01-01' WHERE id = $1",
                        entry_id);
        return 0;
    }),
                 pqxx::sql_error);
}

/// pg_temp-подмена имени journal_lines била по проверке «нельзя провести
/// проводку без строк» (миграция 009, найдена секьюрити-сканом): временная
/// таблица с одной строкой делала NOT EXISTS ложным, и пустая проводка
/// проводилась.
TEST_F(JournalCascadeTest, TempTableNamedJournalLinesCannotPostAnEmptyEntry) {
    const std::string org_id = seed_fully_wired_org();
    QuietLogs quiet;
    std::string empty_id;
    Database::get().execute_write([&](auto& txn) {
        empty_id = txn.exec_params(
                          "INSERT INTO journal_entries (org_id, entry_date, description) "
                          "VALUES ($1, '2026-05-01', 'Без строк') RETURNING id",
                          org_id)
                       .at(0)
                       .at(0)
                       .template as<std::string>();
        return 0;
    });
    EXPECT_THROW(Database::get().execute_write([&](auto& txn) {
        txn.exec_params("CREATE TEMP TABLE journal_lines ON COMMIT DROP AS SELECT $1::uuid AS entry_id", empty_id);
        txn.exec_params("UPDATE journal_entries SET status = 'posted' WHERE id = $1", empty_id);
        return 0;
    }),
                 pqxx::sql_error);
}

TEST_F(JournalCascadeTest, DirectDeleteOfAPostedEntryIsStillRejected) {
    const std::string org_id = seed_fully_wired_org();
    const std::string entry_id = posted_entry_of(org_id);
    QuietLogs quiet;
    EXPECT_THROW(Database::get().execute_write([&](auto& txn) {
        txn.exec_params("DELETE FROM journal_entries WHERE id = $1", entry_id);
        return 0;
    }),
                 pqxx::sql_error);
}

TEST_F(JournalCascadeTest, DirectUpdateOfAPostedEntryIsStillRejected) {
    const std::string org_id = seed_fully_wired_org();
    const std::string entry_id = posted_entry_of(org_id);
    QuietLogs quiet;
    EXPECT_THROW(Database::get().execute_write([&](auto& txn) {
        txn.exec_params("UPDATE journal_entries SET description = 'подделка' WHERE id = $1", entry_id);
        return 0;
    }),
                 pqxx::sql_error);
    // И статус нельзя откатить в draft.
    EXPECT_THROW(Database::get().execute_write([&](auto& txn) {
        txn.exec_params("UPDATE journal_entries SET status = 'draft' WHERE id = $1", entry_id);
        return 0;
    }),
                 pqxx::sql_error);
    // И автора нельзя обнулить, пока пользователь жив: вырез на SET NULL
    // требует, чтобы автора УЖЕ не было в users — иначе это обычная правка
    // проведённой записи приложением.
    EXPECT_THROW(Database::get().execute_write([&](auto& txn) {
        txn.exec_params("UPDATE journal_entries SET created_by_user_id = NULL WHERE id = $1", entry_id);
        return 0;
    }),
                 pqxx::sql_error);
}

TEST_F(JournalCascadeTest, CarveOutUsesNeitherTriggerDepthNorASessionFlag) {
    // Единственный тест, который ловит ЗАПРЕЩЁННЫЕ формы выреза: и
    // pg_trigger_depth() > 1, и сессионный флаг прошли бы все проверки
    // выше зелёными, оставив при этом дыру. Отличить их можно только по
    // телу функции, поэтому оно и читается из каталога.
    //
    // (Тест «wipe_org_data не бросает» здесь стоял и был удалён намеренно:
    // wipe_org_data TRUNCATE'ит journal_entries ДО удаления организаций, а
    // TRUNCATE не запускает построчные триггеры вовсе — тест проходил и
    // без выреза, то есть не мог упасть ни при какой реализации.)
    const std::string src = Database::get().execute_read([](auto& txn) {
        auto r = txn.exec("SELECT prosrc FROM pg_proc WHERE proname = 'journal_entries_immutability'");
        return r.at(0).at(0).template as<std::string>();
    });
    EXPECT_EQ(src.find("pg_trigger_depth"), std::string::npos)
        << "pg_trigger_depth снимает неизменяемость в любом вложенном триггере, а не только в нужном каскаде";
    EXPECT_EQ(src.find("current_setting"), std::string::npos)
        << "сессионный флаг вручает приложению рубильник от insert-only журнала";
    // Разрешённая форма ищется по признаку, а не по точной строке целиком:
    // так тест переживает переформатирование условия и правку комментариев.
    EXPECT_NE(src.find("WHERE id = OLD.org_id"), std::string::npos)
        << "разрешена только форма NOT EXISTS по родительской строке organizations";

    // Квалификация схемой. Без неё pg_temp перехватывает имя отношения (для
    // ОТНОШЕНИЙ временная схема просматривается первой, даже когда её нет в
    // search_path), и CREATE TEMP TABLE organizations обычной ролью снимает
    // неизменяемость журнала целиком. Проверяем отсутствие НЕквалифицированной
    // формы: "FROM organizations" не является подстрокой "FROM public.organizations".
    EXPECT_EQ(src.find("FROM organizations"), std::string::npos)
        << "обращение к organizations обязано быть public.organizations — иначе его перехватывает pg_temp";
    EXPECT_EQ(src.find("FROM users"), std::string::npos)
        << "обращение к users обязано быть public.users — иначе его перехватывает pg_temp";
    EXPECT_EQ(src.find("FROM journal_lines"), std::string::npos)
        << "обращение к journal_lines обязано быть public.journal_lines — иначе его перехватывает pg_temp";

    // Ветка удаления автора обязана сравнивать строку ЦЕЛИКОМ: ручной список
    // колонок (как в 020) оставлял id, created_at и created_by_run_id
    // незакреплёнными и не защищал бы колонку, добавленную в будущем.
    EXPECT_NE(src.find("to_jsonb(NEW)"), std::string::npos)
        << "ветка удалённого автора обязана сравнивать всю строку, а не перечислять колонки";
}

}  // namespace
