import { lazy, type ReactElement } from 'react';
import {
  Shield,
  ScrollText,
  Building2,
  Users,
  BookOpenText,
  FileText,
  UsersRound,
  BriefcaseBusiness,
  Wallet,
  Landmark,
} from 'lucide-react';

import { Permission } from '@/lib/auth/permissions';
// Только тип: манифест и navGroups импортируют друг у друга исключительно
// типы, поэтому циклической рантайм-зависимости не возникает.
import type { NavGroupId } from '@/routes/navGroups';

import { HomePage } from '@/pages/Home';
import { RequisitesPage } from '@/pages/Requisites';
import { AboutPage } from '@/pages/About';
import { LoginPage } from '@/pages/Login';
import { RegisterPage } from '@/pages/Register';
import { CheckEmailPage } from '@/pages/CheckEmail';
import { ConfirmEmailPage } from '@/pages/ConfirmEmail';
import { ConfirmChangeEmailPage } from '@/pages/ConfirmChangeEmail';
import { UnconfirmedPage } from '@/pages/Unconfirmed';
import { ProfilePage } from '@/pages/Profile';
import { ChangePasswordPage } from '@/pages/ChangePassword';
import { ChangeEmailPage } from '@/pages/ChangeEmail';
import { RequestResetPage } from '@/pages/RequestReset';
import { ResetPasswordPage } from '@/pages/ResetPassword';
import { JoinFromInvitePage } from '@/pages/JoinFromInvite';
import { OrganizationsPage } from '@/pages/Organizations';
import { CounterpartiesPage } from '@/pages/Counterparties';
import { JournalPage } from '@/pages/Journal';
import { DocumentsPage } from '@/pages/Documents';
import { GenerateDocumentPage } from '@/pages/GenerateDocument';
import { EmployeesPage } from '@/pages/Employees';
import { HrOrdersPage } from '@/pages/HrOrders';
import { PayrollPage } from '@/pages/Payroll';
import { TaxesPage } from '@/pages/Taxes';

// Admin pages are code-split: a logged-out visitor on /login should not pull
// the whole admin bundle. React.lazy needs a module with a `default` export,
// so each factory maps the page's named export. App.tsx wraps these routes in
// a <Suspense> boundary. (named → default shim kept inline to avoid per-page
// barrel files.)
const AdminDashboardPage = lazy(() =>
  import('@/pages/admin/Dashboard').then((m) => ({ default: m.AdminDashboardPage })),
);
const AdminUsersPage = lazy(() =>
  import('@/pages/admin/Users').then((m) => ({ default: m.AdminUsersPage })),
);
const AdminUserDetailPage = lazy(() =>
  import('@/pages/admin/UserDetail').then((m) => ({ default: m.AdminUserDetailPage })),
);
const AdminInviteUserPage = lazy(() =>
  import('@/pages/admin/InviteUser').then((m) => ({ default: m.AdminInviteUserPage })),
);
const AdminRolesPage = lazy(() =>
  import('@/pages/admin/Roles').then((m) => ({ default: m.AdminRolesPage })),
);
const AdminJobsPage = lazy(() =>
  import('@/pages/admin/Jobs').then((m) => ({ default: m.AdminJobsPage })),
);
const AdminAuditPage = lazy(() =>
  import('@/pages/admin/Audit').then((m) => ({ default: m.AdminAuditPage })),
);
/**
 * Single routes manifest — THE source of truth for both:
 *   - App.tsx, which renders <Route>s grouped by `guard`, and
 *   - Nav.tsx, which renders the (permission-filtered) nav links.
 *
 * Keeping route ↔ nav in one array kills the drift where a page was
 * mounted but never linked (or linked but mounted under the wrong guard).
 * Add a page by appending one entry here; `scripts/new-react-page.sh`
 * automates the append.
 *
 * Guard semantics (mirror App.tsx's old layout-route groups 1:1):
 *   - 'public'    — no auth required.
 *   - 'auth'      — any signed-in user (confirmed or not). Used by
 *                   /unconfirmed so an unconfirmed user can still land.
 *   - 'confirmed' — signed in AND email-confirmed.
 *   - 'admin'     — signed in, confirmed, AND Permission.Administer
 *                   (the 0x40000000 sentinel bit, not 0xff).
 *
 * `navLabel` opts a route into the top nav. `navIcon` (a lucide icon
 * component) renders before the label. Routes with a dynamic `:param`
 * segment never belong in the nav, so they simply omit `navLabel`.
 *
 * `requirePermission` lets a non-admin route still gate on a specific
 * bit; admin routes imply Permission.Administer via their guard, so they
 * don't repeat it.
 *
 * `navGroup` + `navRoles` drive the grouped, role-filtered top nav — see
 * `@/routes/navGroups`, which owns the section list AND their order. The
 * order of entries in THIS array never decides section order.
 */
export type RouteGuard = 'public' | 'auth' | 'confirmed' | 'admin';

export interface RouteEntry {
  path: string;
  element: ReactElement;
  guard: RouteGuard;
  /** When present, the route shows up in the top nav with this label. */
  navLabel?: string;
  /** Optional lucide icon component rendered before the nav label. */
  navIcon?: React.ComponentType<{ className?: string }>;
  /** Extra permission bit a non-admin route must carry (rare). */
  requirePermission?: number;
  /** Раздел верхнего меню; отсутствие поля = ссылка рисуется плоско. */
  navGroup?: NavGroupId;
  /**
   * Роли в организации (org_members.role), которым ссылка видна.
   * Отсутствие = всем. Значения — по матрице прав P3 §5.3
   * (src/tenancy/OrgPermissions.hpp): «—» в таблице означает НЕВИДИМО, а
   * не «только чтение», поэтому пункт, на который сервер ответит 403,
   * в меню не появляется вовсе.
   */
  navRoles?: readonly string[];
}

export const routes: RouteEntry[] = [
  // ── Public ────────────────────────────────────────────────────────────
  { path: '/', element: <HomePage />, guard: 'public', navLabel: 'Главная' },
  { path: '/about', element: <AboutPage />, guard: 'public', navLabel: 'О сервисе' },
  { path: '/login', element: <LoginPage />, guard: 'public' },
  { path: '/register', element: <RegisterPage />, guard: 'public' },
  { path: '/account/check-email', element: <CheckEmailPage />, guard: 'public' },
  { path: '/account/confirm/:token', element: <ConfirmEmailPage />, guard: 'public' },
  // Public on purpose: the link lands in the NEW mailbox, where the user
  // may not have a session. The token itself authenticates. v6 ranks the
  // static /account/change-email (confirmed, below) above this :token
  // segment, so they don't conflict.
  {
    path: '/account/change-email/:token',
    element: <ConfirmChangeEmailPage />,
    guard: 'public',
  },
  { path: '/account/reset-password', element: <RequestResetPage />, guard: 'public' },
  { path: '/account/reset-password/:token', element: <ResetPasswordPage />, guard: 'public' },
  {
    path: '/account/join-from-invite/:token',
    element: <JoinFromInvitePage />,
    guard: 'public',
  },

  // ── Authenticated but possibly unconfirmed ──────────────────────────────
  { path: '/unconfirmed', element: <UnconfirmedPage />, guard: 'auth' },

  // ── Authenticated + confirmed ───────────────────────────────────────────
  { path: '/account', element: <ProfilePage />, guard: 'confirmed' },
  { path: '/account/change-password', element: <ChangePasswordPage />, guard: 'confirmed' },
  { path: '/account/change-email', element: <ChangeEmailPage />, guard: 'confirmed' },

  // ── Admin — gated by Permission.Administer (0x40000000 sentinel) ────────
  {
    path: '/admin',
    element: <AdminDashboardPage />,
    guard: 'admin',
    navLabel: 'Администрирование',
    navIcon: Shield,
    navGroup: 'settings',
  },
  { path: '/admin/users', element: <AdminUsersPage />, guard: 'admin' },
  { path: '/admin/users/:id', element: <AdminUserDetailPage />, guard: 'admin' },
  { path: '/admin/invite', element: <AdminInviteUserPage />, guard: 'admin' },
  { path: '/admin/roles', element: <AdminRolesPage />, guard: 'admin' },
  { path: '/admin/jobs', element: <AdminJobsPage />, guard: 'admin' },

  // ── Audit — read-only, gated on kAuditRead (0x02) rather than full ──────
  // Administer. A 'confirmed' guard + requirePermission means App.tsx wraps
  // it in a ProtectedRoute for the AuditRead bit and Nav filters the link by
  // it; full admins (the Administer sentinel) satisfy every check, so they
  // see it too.
  {
    path: '/admin/audit',
    element: <AdminAuditPage />,
    guard: 'confirmed',
    requirePermission: Permission.AuditRead,
    navLabel: 'Аудит',
    navIcon: ScrollText,
    navGroup: 'settings',
  },
  {
    path: '/organizations',
    element: <OrganizationsPage />,
    guard: 'confirmed',
    navLabel: 'Организации',
    navIcon: Building2,
    navGroup: 'settings',
    // navRoles намеренно не задан: список СВОИХ организаций видит любая
    // роль, включая кадровика, — это и есть точка входа в смену тенанта.
  },
  {
    path: '/requisites',
    element: <RequisitesPage />,
    guard: 'confirmed',
    navLabel: 'Реквизиты',
    navIcon: Landmark,
    navGroup: 'settings',
    // Кадровик сюда не ходит: реквизиты и счета к кадровым документам
    // отношения не имеют (матрица прав, ресурс `requisites`).
    navRoles: ['owner', 'accountant', 'viewer'],
  },
  {
    path: '/counterparties',
    element: <CounterpartiesPage />,
    guard: 'confirmed',
    navLabel: 'Контрагенты',
    navIcon: Users,
    navGroup: 'accounting',
    navRoles: ['owner', 'accountant', 'viewer'],
  },
  {
    path: '/journal',
    element: <JournalPage />,
    guard: 'confirmed',
    navLabel: 'Журнал проводок',
    navIcon: BookOpenText,
    navGroup: 'accounting',
    navRoles: ['owner', 'accountant', 'viewer'],
  },
  {
    path: '/documents',
    element: <DocumentsPage />,
    guard: 'confirmed',
    navLabel: 'Документы',
    navIcon: FileText,
    navGroup: 'accounting',
    // ОСОЗНАННОЕ РАСХОЖДЕНИЕ С МАТРИЦЕЙ: сервер кадровику реестр документов
    // отдаёт (200, сузив выдачу до doc_type='hr'), но сама страница делает
    // безусловный запрос контрагентов, который для 'hr' — 403, то есть
    // сегодня она у него сломана. Пока пункт скрыт; вернуть 'hr' в этот
    // список ровно тогда, когда страница станет hr-безопасной
    // (отдельная задача — сделать запрос контрагентов условным).
    navRoles: ['owner', 'accountant', 'viewer'],
  },
  // No navLabel: reached from DocumentsPage's "Создать документ" button,
  // not a top-level nav destination.
  { path: '/documents/generate', element: <GenerateDocumentPage />, guard: 'confirmed' },
  {
    path: '/employees',
    element: <EmployeesPage />,
    guard: 'confirmed',
    navLabel: 'Сотрудники',
    navIcon: UsersRound,
    navGroup: 'people',
    navRoles: ['owner', 'accountant', 'hr', 'viewer'],
  },
  {
    path: '/hr',
    element: <HrOrdersPage />,
    guard: 'confirmed',
    navLabel: 'Кадры',
    navIcon: BriefcaseBusiness,
    navGroup: 'people',
    navRoles: ['owner', 'accountant', 'hr', 'viewer'],
  },
  {
    path: '/payroll',
    element: <PayrollPage />,
    guard: 'confirmed',
    navLabel: 'Зарплата',
    navIcon: Wallet,
    navGroup: 'people',
    navRoles: ['owner', 'accountant', 'viewer'],
  },
  {
    path: '/taxes',
    element: <TaxesPage />,
    guard: 'confirmed',
    navLabel: 'Налоги',
    navIcon: Landmark,
    navGroup: 'tax',
    navRoles: ['owner', 'accountant', 'viewer'],
  },
];

/** The permission a guard implies, for nav-link filtering in Nav.tsx. */
export function guardPermission(entry: RouteEntry): number {
  if (entry.guard === 'admin') return Permission.Administer;
  return entry.requirePermission ?? Permission.None;
}
