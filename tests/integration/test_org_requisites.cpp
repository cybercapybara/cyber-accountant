/**
 * @file test_org_requisites.cpp
 * @brief Реквизиты организации и её расчётные счета против настоящего
 *        Postgres (migrations/025_org_requisites.sql).
 *
 * Главное здесь — не CRUD, а три инварианта, которые дороже него:
 *   1. «Основной счёт ровно один» держит БАЗА (частичный уникальный индекс),
 *      а не порядок вызовов в контроллере.
 *   2. Назначение основного снимает прежний В ТОЙ ЖЕ транзакции — иначе
 *      пользователь получил бы 409 вместо ожидаемого «теперь основной этот».
 *   3. Счета одной организации недостижимы из другой.
 */

#include <exception>
#include <string>

#include <gtest/gtest.h>

#include "database/Database.hpp"
#include "tenancy/BankAccountRepository.hpp"
#include "tenancy/OrganizationRepository.hpp"
#include "test_helpers.hpp"

namespace {

class OrgRequisitesTest : public TestHelpers::CoreBackedTest {
protected:
    void SetUp() override {
        TestHelpers::CoreBackedTest::SetUp();
        if (::testing::Test::IsSkipped())
            return;
        TestHelpers::wipe_org_data();
    }

    std::string make_org(const std::string& bin) {
        Tenancy::OrganizationRepository orgs;
        return orgs.create(bin, "Requisites Test Org " + bin, "snr_simplified", false).id;
    }

    static Tenancy::BankAccount draft(const std::string& iik, bool primary = false) {
        Tenancy::BankAccount a;
        a.iik = iik;
        a.bank_name = "АО «Банк ЦентрКредит»";
        a.bik = "KCJBKZKX";
        a.kbe = "17";
        a.currency = "KZT";
        a.is_primary = primary;
        return a;
    }
};

// --------------------------------------------------------------------------
// Реквизиты организации
// --------------------------------------------------------------------------

TEST_F(OrgRequisitesTest, NewOrganizationStartsWithEmptyRequisitesNotNulls) {
    // Колонки объявлены NOT NULL DEFAULT '', поэтому «не заполнено» — это
    // пустая строка, а не NULL. Подстановка в документ обязана различать эти
    // случаи одинаково, и тест фиксирует, что различать нечего.
    Tenancy::OrganizationRepository orgs;
    auto org = orgs.create("111240000101", "Fresh Org", "snr_simplified", false);
    EXPECT_EQ(org.legal_address, "");
    EXPECT_EQ(org.director_name, "");
    EXPECT_EQ(org.director_position, "Директор");
}

TEST_F(OrgRequisitesTest, RequisitesRoundTripThroughTheRepository) {
    Tenancy::OrganizationRepository orgs;
    auto org_id = make_org("111240000102");

    ASSERT_TRUE(orgs.update_requisites(org_id, "г. Алматы, пр. Абая 1", "Тарасов М.", "Генеральный директор"));

    auto reloaded = orgs.find(org_id);
    ASSERT_TRUE(reloaded.has_value());
    EXPECT_EQ(reloaded->legal_address, "г. Алматы, пр. Абая 1");
    EXPECT_EQ(reloaded->director_name, "Тарасов М.");
    EXPECT_EQ(reloaded->director_position, "Генеральный директор");
}

TEST_F(OrgRequisitesTest, RequisitesCanBeClearedBackToEmpty) {
    // Пустая строка — законное значение, а не «не трогать». Если бы очистка
    // молча игнорировалась, ошибочно введённого директора нельзя было бы
    // убрать, и он печатался бы в документах дальше.
    Tenancy::OrganizationRepository orgs;
    auto org_id = make_org("111240000103");
    ASSERT_TRUE(orgs.update_requisites(org_id, "адрес", "директор", "должность"));

    ASSERT_TRUE(orgs.update_requisites(org_id, "", "", ""));
    auto reloaded = orgs.find(org_id);
    ASSERT_TRUE(reloaded.has_value());
    EXPECT_EQ(reloaded->legal_address, "");
    EXPECT_EQ(reloaded->director_name, "");
}

TEST_F(OrgRequisitesTest, UpdatingRequisitesOfAMissingOrganizationIsFalseNotThrow) {
    Tenancy::OrganizationRepository orgs;
    EXPECT_FALSE(orgs.update_requisites("00000000-0000-0000-0000-000000000000", "a", "b", "c"));
}

// --------------------------------------------------------------------------
// Расчётные счета
// --------------------------------------------------------------------------

TEST_F(OrgRequisitesTest, CreateAndListPutsThePrimaryAccountFirst) {
    Tenancy::BankAccountRepository repo;
    auto org_id = make_org("111240000104");

    repo.create(org_id, draft("KZ11111111111111111"));
    repo.create(org_id, draft("KZ22222222222222222", /*primary=*/true));

    auto rows = repo.list_in_org(org_id, 50, 0);
    ASSERT_EQ(rows.size(), 2u);
    // Основной идёт первым: он же подставляется в документ по умолчанию, и
    // список, где он не наверху, вводит в заблуждение.
    EXPECT_EQ(rows[0].iik, "KZ22222222222222222");
    EXPECT_TRUE(rows[0].is_primary);
}

TEST_F(OrgRequisitesTest, TheSameIikTwiceInOneOrgIsAConflict) {
    Tenancy::BankAccountRepository repo;
    auto org_id = make_org("111240000105");
    repo.create(org_id, draft("KZ33333333333333333"));

    EXPECT_THROW(repo.create(org_id, draft("KZ33333333333333333")), Tenancy::DuplicateBankAccount);
}

TEST_F(OrgRequisitesTest, TheSameIikIsAllowedInTwoDifferentOrganizations) {
    // UNIQUE — по паре (org_id, iik). Глобальная уникальность была бы ошибкой:
    // два арендатора площадки — это разные ТОО, и совпадение их счёта в базе
    // не должно мешать ни одному из них.
    Tenancy::BankAccountRepository repo;
    auto a = make_org("111240000106");
    auto b = make_org("111240000107");

    repo.create(a, draft("KZ44444444444444444"));
    EXPECT_NO_THROW(repo.create(b, draft("KZ44444444444444444")));
}

TEST_F(OrgRequisitesTest, MarkingAnAccountPrimaryDemotesThePreviousOne) {
    // Это и есть причина, по которой снятие и установка лежат в одной
    // транзакции: без него частичный уникальный индекс отверг бы вставку, и
    // владелец увидел бы 409 на совершенно законное действие.
    Tenancy::BankAccountRepository repo;
    auto org_id = make_org("111240000108");
    auto first = repo.create(org_id, draft("KZ55555555555555555", /*primary=*/true));

    auto second = repo.create(org_id, draft("KZ66666666666666666", /*primary=*/true));

    auto primary = repo.find_primary(org_id);
    ASSERT_TRUE(primary.has_value());
    EXPECT_EQ(primary->id, second.id);

    auto demoted = repo.find_in_org(first.id, org_id);
    ASSERT_TRUE(demoted.has_value());
    EXPECT_FALSE(demoted->is_primary);
}

TEST_F(OrgRequisitesTest, PromotingViaUpdateAlsoDemotesThePreviousPrimary) {
    Tenancy::BankAccountRepository repo;
    auto org_id = make_org("111240000109");
    auto first = repo.create(org_id, draft("KZ77777777777777777", /*primary=*/true));
    auto second = repo.create(org_id, draft("KZ88888888888888888"));

    auto patch = second;
    patch.is_primary = true;
    auto updated = repo.update(org_id, second.id, patch);
    ASSERT_TRUE(updated.has_value());
    EXPECT_TRUE(updated->is_primary);

    auto demoted = repo.find_in_org(first.id, org_id);
    ASSERT_TRUE(demoted.has_value());
    EXPECT_FALSE(demoted->is_primary);
}

TEST_F(OrgRequisitesTest, TwoPrimaryAccountsAreImpossibleEvenBypassingTheRepository) {
    // Инвариант держит БАЗА, а не код репозитория. Проверяем это прямым
    // UPDATE в обход clear_primary: если бы гарантия жила только в C++,
    // любой другой путь записи (миграция данных, ручная правка, будущий
    // сервис) молча создал бы вторую «основную» строку, и подстановка счёта
    // в документ стала бы недетерминированной.
    Tenancy::BankAccountRepository repo;
    auto org_id = make_org("111240000110");
    repo.create(org_id, draft("KZ99999999999999999", /*primary=*/true));
    auto second = repo.create(org_id, draft("KZ10101010101010101"));

    EXPECT_THROW(
        {
            Database::get().execute_write([&](auto& txn) {
                txn.exec_params("UPDATE bank_accounts SET is_primary = TRUE WHERE id = $1", second.id);
            });
        },
        std::exception);
}

TEST_F(OrgRequisitesTest, AccountsOfOneOrgAreInvisibleToAnother) {
    Tenancy::BankAccountRepository repo;
    auto mine = make_org("111240000111");
    auto theirs = make_org("111240000112");
    auto account = repo.create(mine, draft("KZ12121212121212121"));

    EXPECT_FALSE(repo.find_in_org(account.id, theirs).has_value());
    EXPECT_EQ(repo.count_in_org(theirs), 0);
    // И удалить чужой счёт тоже нельзя — id один, а пара (id, org_id) не та.
    EXPECT_FALSE(repo.remove(theirs, account.id));
    EXPECT_TRUE(repo.find_in_org(account.id, mine).has_value());
}

TEST_F(OrgRequisitesTest, DeletingAnAccountIsScopedAndIdempotentlyFalse) {
    Tenancy::BankAccountRepository repo;
    auto org_id = make_org("111240000113");
    auto account = repo.create(org_id, draft("KZ13131313131313131"));

    EXPECT_TRUE(repo.remove(org_id, account.id));
    EXPECT_FALSE(repo.remove(org_id, account.id));
}

TEST_F(OrgRequisitesTest, NoPrimaryAccountIsAnEmptyOptionalNotAnError) {
    // Организация без назначенного основного счёта — нормальное состояние, и
    // подстановка в документ обязана уметь его пережить, а не упасть.
    Tenancy::BankAccountRepository repo;
    auto org_id = make_org("111240000114");
    repo.create(org_id, draft("KZ14141414141414141"));

    EXPECT_FALSE(repo.find_primary(org_id).has_value());
}

}  // namespace
