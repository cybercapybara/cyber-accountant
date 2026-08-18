-- Фундамент ИИ-агента: роль, прогоны, шаги, политики автономии, одобрения
-- (спека P4 §3.1, §4, §5, §6).
--
-- ПОРЯДОК ВАЖЕН, и урок здесь чужой, но оплаченный: migrations/017 прямо
-- предупреждает, что расширять множество ролей в БД РАНЬШЕ, чем матрица прав о
-- новой роли знает, — это открытие настежь. Колонка `agent` добавлена в
-- src/tenancy/OrgPermissions.hpp в ЭТОМ ЖЕ коммите; принять роль в базе, не
-- объявив её полномочий, значило бы выдать неизвестной роли всё, что не
-- закрыто явно.
--
-- NOTE: MigrationRunner оборачивает файл в ОДНУ транзакцию. Без BEGIN/COMMIT.

ALTER TABLE org_members DROP CONSTRAINT IF EXISTS org_members_role_check;
ALTER TABLE org_members ADD CONSTRAINT org_members_role_check
    CHECK (role IN ('owner', 'accountant', 'hr', 'viewer', 'agent'));

-- ---------------------------------------------------------------------------
-- Прогоны и шаги: полный след того, что агент делал и почему.
--
-- Это не отладка, а условие доверия: полуавтономный бухгалтер, чьи действия
-- нельзя разобрать постфактум, неотличим от случайных изменений в учёте.
-- ---------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS agent_runs (
    id           UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    org_id       UUID NOT NULL REFERENCES organizations(id) ON DELETE CASCADE,
    trigger      TEXT NOT NULL CHECK (trigger IN ('chat', 'schedule', 'event')),
    -- Кто инициировал. NULL для расписания и событий: там инициатора-человека
    -- нет, и записывать туда кого-то «за компанию» значило бы подделать след.
    started_by   UUID REFERENCES users(id) ON DELETE SET NULL,
    status       TEXT NOT NULL DEFAULT 'running'
                 CHECK (status IN ('running', 'succeeded', 'failed', 'awaiting_approval', 'budget_exceeded')),
    summary      TEXT NOT NULL DEFAULT '',
    -- Токены и стоимость: бюджет на организацию (§9) считается по ним, а не по
    -- числу прогонов — прогон бывает и в один вызов, и в сорок.
    input_tokens  BIGINT NOT NULL DEFAULT 0,
    output_tokens BIGINT NOT NULL DEFAULT 0,
    cost_tiyn     BIGINT NOT NULL DEFAULT 0,
    error        TEXT,
    created_at   TIMESTAMPTZ NOT NULL DEFAULT now(),
    updated_at   TIMESTAMPTZ NOT NULL DEFAULT now()
);
CREATE INDEX IF NOT EXISTS idx_agent_runs_org ON agent_runs (org_id, created_at DESC);

CREATE TABLE IF NOT EXISTS agent_steps (
    id         UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    run_id     UUID NOT NULL REFERENCES agent_runs(id) ON DELETE CASCADE,
    seq        INT  NOT NULL,
    kind       TEXT NOT NULL CHECK (kind IN ('prompt', 'model_reply', 'tool_call', 'tool_result', 'error')),
    tool       TEXT,
    payload    JSONB NOT NULL DEFAULT '{}'::jsonb,
    -- Вердикт движка политик по этому шагу: что решили и почему. Пишется даже
    -- когда действие разрешено, иначе «почему это прошло автоматически»
    -- останется без ответа ровно тогда, когда ответ понадобится.
    policy_decision TEXT CHECK (policy_decision IN ('auto', 'approve', 'forbid')),
    policy_reason   TEXT,
    created_at TIMESTAMPTZ NOT NULL DEFAULT now(),
    UNIQUE (run_id, seq)
);
CREATE INDEX IF NOT EXISTS idx_agent_steps_run ON agent_steps (run_id, seq);

-- ---------------------------------------------------------------------------
-- Политики автономии: тип действия x границы x режим.
--
-- УМОЛЧАНИЕ ЖИВЁТ В КОДЕ, А НЕ ЗДЕСЬ. Отсутствие строки означает `approve`
-- (спека §4): действие, для которого политики не завели, требует человека.
-- Класть умолчание в таблицу нельзя — тогда пустая таблица означала бы «нет
-- правил», а это ровно то состояние, в котором система живёт до первой
-- настройки.
-- ---------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS autonomy_policies (
    id          UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    org_id      UUID NOT NULL REFERENCES organizations(id) ON DELETE CASCADE,
    action      TEXT NOT NULL,
    mode        TEXT NOT NULL CHECK (mode IN ('auto', 'approve', 'forbid')),
    -- Границы. NULL = граница не задана, а НЕ «ноль»: max_amount_tiyn = 0
    -- означало бы «только бесплатные действия», и путать это с «без предела»
    -- нельзя.
    max_amount_tiyn      BIGINT CHECK (max_amount_tiyn IS NULL OR max_amount_tiyn >= 0),
    counterparty_allowlist UUID[],
    doc_type    TEXT,
    -- «Такое же действие уже одобряли руками» — послабление, которое включают
    -- явно, а не выводят из истории само собой.
    require_prior_approval BOOLEAN NOT NULL DEFAULT FALSE,
    created_at  TIMESTAMPTZ NOT NULL DEFAULT now(),
    updated_at  TIMESTAMPTZ NOT NULL DEFAULT now(),
    UNIQUE (org_id, action, doc_type)
);
CREATE INDEX IF NOT EXISTS idx_autonomy_policies_org ON autonomy_policies (org_id, action);

-- ---------------------------------------------------------------------------
-- Запросы одобрения.
--
-- `human_diff` — обязательное поле, а не удобство. Одобряют ДЕЙСТВИЕ, а не
-- JSON: «провести 12 500,00 ₸ по счёту от ТОО „Ромашка“» человек прочитает, а
-- {"lines":[{"account":"1030",...}]} — нет, и одобрение выродится в нажатие
-- кнопки не глядя.
-- ---------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS approval_requests (
    id          UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    org_id      UUID NOT NULL REFERENCES organizations(id) ON DELETE CASCADE,
    run_id      UUID REFERENCES agent_runs(id) ON DELETE CASCADE,
    action      TEXT NOT NULL,
    payload     JSONB NOT NULL,
    human_diff  TEXT NOT NULL,
    status      TEXT NOT NULL DEFAULT 'pending'
                CHECK (status IN ('pending', 'approved', 'rejected', 'expired')),
    decided_by  UUID REFERENCES users(id) ON DELETE SET NULL,
    decided_at  TIMESTAMPTZ,
    comment     TEXT NOT NULL DEFAULT '',
    expires_at  TIMESTAMPTZ,
    created_at  TIMESTAMPTZ NOT NULL DEFAULT now(),
    updated_at  TIMESTAMPTZ NOT NULL DEFAULT now(),
    -- Решение и решивший приходят вместе. Строка со статусом `approved` без
    -- отметки, КТО одобрил, — это одобрение без ответственного, то есть ровно
    -- то, чего инбокс и должен не допускать.
    CONSTRAINT ck_approval_decided_together CHECK (
        (status IN ('pending', 'expired') AND decided_by IS NULL AND decided_at IS NULL)
        OR (status IN ('approved', 'rejected') AND decided_by IS NOT NULL AND decided_at IS NOT NULL)
    )
);
CREATE INDEX IF NOT EXISTS idx_approval_requests_pending
    ON approval_requests (org_id, created_at DESC) WHERE status = 'pending';

DROP TRIGGER IF EXISTS trg_agent_runs_touch ON agent_runs;
CREATE TRIGGER trg_agent_runs_touch BEFORE UPDATE ON agent_runs
    FOR EACH ROW EXECUTE FUNCTION public.touch_updated_at();
DROP TRIGGER IF EXISTS trg_autonomy_policies_touch ON autonomy_policies;
CREATE TRIGGER trg_autonomy_policies_touch BEFORE UPDATE ON autonomy_policies
    FOR EACH ROW EXECUTE FUNCTION public.touch_updated_at();
DROP TRIGGER IF EXISTS trg_approval_requests_touch ON approval_requests;
CREATE TRIGGER trg_approval_requests_touch BEFORE UPDATE ON approval_requests
    FOR EACH ROW EXECUTE FUNCTION public.touch_updated_at();
