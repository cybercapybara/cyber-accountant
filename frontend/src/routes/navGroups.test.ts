import { describe, expect, it } from 'vitest';

import type { PermissionUser } from '@/lib/auth/permissions';
import { routes } from '@/routes/manifest';
import { groupNavLinks, ungroupedNavLinks, NAV_GROUPS } from '@/routes/navGroups';

/**
 * Таблица «роль → что видно» — зеркало матрицы P3 §5.3
 * (src/tenancy/OrgPermissions.hpp), где авторитет — сервер. Утверждается и
 * то, что роль видит, и то, чего она видеть НЕ ДОЛЖНА: пункт меню, на
 * который сервер отвечает 403, хуже отсутствующего пункта.
 */
const plainUser = { role: { permissions: 0x01 } } as never;
const adminUser = { role: { permissions: 0x40000000 } } as never;

/** Все ссылки, видимые роли, в одном плоском списке путей. */
const visiblePaths = (user: PermissionUser | null, orgRole: string | null) =>
  [
    ...ungroupedNavLinks(routes, user, orgRole),
    ...groupNavLinks(routes, user, orgRole).flatMap((g) => g.links),
  ]
    .map((r) => r.path)
    .sort();

describe('groupNavLinks', () => {
  it('keeps the declared group order regardless of route order', () => {
    const ids = groupNavLinks(routes, adminUser, 'owner').map((g) => g.id);
    expect(ids).toEqual(NAV_GROUPS.filter((g) => ids.includes(g.id)).map((g) => g.id));
  });

  it('gives the owner all four groups and ten links', () => {
    const groups = groupNavLinks(routes, adminUser, 'owner');
    expect(groups.map((g) => g.id)).toEqual(['accounting', 'people', 'tax', 'settings']);
    expect(groups.reduce((n, g) => n + g.links.length, 0)).toBe(10);
  });

  it('shows the hr role the people group without payroll, plus settings', () => {
    const groups = groupNavLinks(routes, plainUser, 'hr');
    expect(groups.map((g) => g.id)).toEqual(['people', 'settings']);
    const people = groups.find((g) => g.id === 'people');
    expect(people?.links.map((l) => l.path).sort()).toEqual(['/employees', '/hr']);
    // Зарплата кадровику невидима, а не «только для чтения».
    expect(people?.links.some((l) => l.path === '/payroll')).toBe(false);
  });

  it('drops empty groups entirely', () => {
    const groups = groupNavLinks(routes, plainUser, null);
    expect(groups.every((g) => g.links.length > 0)).toBe(true);
    expect(groups.map((g) => g.id)).not.toContain('accounting');
  });

  it('leaves the public links ungrouped', () => {
    expect(ungroupedNavLinks(routes, null, null).map((r) => r.path)).toEqual(['/', '/about']);
  });
});

describe('role → visibility matrix (mirrors OrgPermissions.hpp §5.3)', () => {
  const accounting = ['/counterparties', '/documents', '/journal'];
  const people = ['/employees', '/hr'];
  const publicLinks = ['/', '/about'];

  const cases: { role: string; sees: string[] }[] = [
    {
      role: 'owner',
      sees: [...publicLinks, ...accounting, ...people, '/payroll', '/taxes', '/organizations'],
    },
    {
      role: 'accountant',
      sees: [...publicLinks, ...accounting, ...people, '/payroll', '/taxes', '/organizations'],
    },
    // Кадровик: сотрудники и кадровые документы — да; зарплата, журнал,
    // налоги, контрагенты и первичка — нет (в матрице у него пустая ячейка).
    { role: 'hr', sees: [...publicLinks, ...people, '/organizations'] },
    {
      role: 'viewer',
      sees: [...publicLinks, ...accounting, ...people, '/payroll', '/taxes', '/organizations'],
    },
    // Неизвестная роль и отсутствие роли закрыты так же, как на сервере
    // (запрет по умолчанию): остаются только публичные ссылки и список
    // своих организаций.
    { role: 'nope', sees: [...publicLinks, '/organizations'] },
  ];

  for (const { role, sees } of cases) {
    it(`shows the ${role} role exactly its allowed links`, () => {
      expect(visiblePaths(plainUser, role)).toEqual([...sees].sort());
    });

    it(`hides everything else from the ${role} role`, () => {
      const denied = routes
        .filter((r) => r.navLabel && !sees.includes(r.path))
        .map((r) => r.path)
        .sort();
      const visible = visiblePaths(plainUser, role);
      for (const path of denied) expect(visible).not.toContain(path);
      // Админские пункты гейтятся глобальным битом, а не org-ролью: у
      // пользователя без Permission.Administer / AuditRead их нет ни при
      // какой роли в организации.
      expect(denied).toEqual(expect.arrayContaining(['/admin', '/admin/audit']));
    });
  }

  it('gives a null org role only the links no role gates', () => {
    expect(visiblePaths(plainUser, null)).toEqual(['/', '/about', '/organizations'].sort());
  });

  it('adds the admin-only links for a user carrying the Administer bit', () => {
    expect(visiblePaths(adminUser, 'owner')).toContain('/admin');
    expect(visiblePaths(adminUser, 'owner')).toContain('/admin/audit');
  });
});
