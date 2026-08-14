import { z } from 'zod';

/**
 * Mirrors Validation::* on the backend (src/api/Validation.hpp + the
 * register/login/reset handlers). zod is the source of truth on the
 * client; the backend re-validates on every request.
 */

export const loginSchema = z.object({
  email: z.string().min(1, 'Укажите email').email('Некорректный email'),
  password: z.string().min(1, 'Укажите пароль'),
});

export const registerSchema = z
  .object({
    email: z.string().min(1, 'Укажите email').email('Некорректный email'),
    password: z
      .string()
      .min(8, 'Пароль должен быть не короче 8 символов')
      .max(128, 'Пароль слишком длинный'),
    password_confirm: z.string(),
    first_name: z.string().max(64).optional(),
    last_name: z.string().max(64).optional(),
  })
  .refine((d) => d.password === d.password_confirm, {
    path: ['password_confirm'],
    message: 'Пароли должны совпадать',
  });

export const requestResetSchema = z.object({
  email: z.string().min(1, 'Укажите email').email('Некорректный email'),
});

export const resetPasswordSchema = z
  .object({
    new_password: z
      .string()
      .min(8, 'Пароль должен быть не короче 8 символов')
      .max(128, 'Пароль слишком длинный'),
    new_password_confirm: z.string(),
  })
  .refine((d) => d.new_password === d.new_password_confirm, {
    path: ['new_password_confirm'],
    message: 'Пароли должны совпадать',
  });

export const changePasswordSchema = z
  .object({
    old_password: z.string().min(1, 'Укажите текущий пароль'),
    new_password: z
      .string()
      .min(8, 'Пароль должен быть не короче 8 символов')
      .max(128, 'Пароль слишком длинный'),
    new_password_confirm: z.string(),
  })
  .refine((d) => d.new_password === d.new_password_confirm, {
    path: ['new_password_confirm'],
    message: 'Пароли должны совпадать',
  });

export const changeEmailSchema = z.object({
  new_email: z.string().min(1, 'Укажите email').email('Некорректный email'),
  password: z.string().min(1, 'Укажите пароль'),
});
