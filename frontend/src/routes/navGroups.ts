/**
 * Разделы верхнего меню (спека P3 §6). Порядок разделов задаётся ЗДЕСЬ,
 * а не порядком маршрутов в манифесте: иначе перестановка страницы молча
 * переставляла бы раздел.
 *
 * Десять пунктов авторизованного меню плоским списком читались как свалка,
 * поэтому они собраны в четыре раздела: «Учёт», «Кадры и зарплата»,
 * «Налоги», «Настройки». «Главная» и «О сервисе» в разделы не входят и
 * рисуются плоско перед ними.
 */
import { userCan, type PermissionUser } from '@/lib/auth/permissions';
import { guardPermission, type RouteEntry } from '@/routes/manifest';

export type NavGroupId = 'accounting' | 'people' | 'tax' | 'settings';

export interface NavGroup {
  id: NavGroupId;
  label: string;
}

/** Упорядоченный список разделов — единственный источник их порядка. */
export const NAV_GROUPS: readonly NavGroup[] = [
  { id: 'accounting', label: 'Учёт' },
  { id: 'people', label: 'Кадры и зарплата' },
  { id: 'tax', label: 'Налоги' },
  // Раздел из одного пункта — осознанно: расчёты, сроки и ФНО внутри
  // /taxes являются вкладками страницы, а не отдельными маршрутами.
  { id: 'settings', label: 'Настройки' },
] as const;

/** Раздел с уже отфильтрованными ссылками; пустых наружу не выдаём. */
export interface NavGroupWithLinks extends NavGroup {
  links: RouteEntry[];
}

/**
 * Двухфакторный предикат: глобальный бит прав пользователя И роль в
 * организации. Маршрут без navRoles ролью не ограничен; маршрут с
 * navRoles невидим, пока роль неизвестна (orgRole === null) — fail-closed,
 * как и на сервере (Tenancy::OrgPerm::allows, запрет по умолчанию).
 *
 * Публичные маршруты («Главная», «О сервисе») проверку прав не проходят
 * вовсе: userCan(null, …) — false для любого бита, включая Permission.None,
 * а прятать публичные ссылки от неавторизованного посетителя незачем.
 */
export function isNavLinkVisible(
  route: RouteEntry,
  user: PermissionUser | null,
  orgRole: string | null,
): boolean {
  if (!route.navLabel) return false;
  if (route.guard !== 'public' && !userCan(user, guardPermission(route))) return false;
  if (!route.navRoles) return true;
  return orgRole !== null && route.navRoles.includes(orgRole);
}

/** Ссылки без группы (Главная, О сервисе) — рисуются плоско перед разделами. */
export function ungroupedNavLinks(
  routes: RouteEntry[],
  user: PermissionUser | null,
  orgRole: string | null,
): RouteEntry[] {
  return routes.filter((r) => !r.navGroup && isNavLinkVisible(r, user, orgRole));
}

/**
 * Разделы в порядке NAV_GROUPS; ПУСТЫЕ УЖЕ ОТФИЛЬТРОВАНЫ. Оба рендера
 * Nav.tsx (десктоп и мобильная панель) обязаны рисовать этот массив как
 * есть и не повторять фильтрацию в JSX — иначе одну из двух копий забудут.
 */
export function groupNavLinks(
  routes: RouteEntry[],
  user: PermissionUser | null,
  orgRole: string | null,
): NavGroupWithLinks[] {
  return NAV_GROUPS.map((g) => ({
    ...g,
    links: routes.filter((r) => r.navGroup === g.id && isNavLinkVisible(r, user, orgRole)),
  })).filter((g) => g.links.length > 0);
}
