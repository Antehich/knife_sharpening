/*
 * Оценка вектора гравитации, устойчивая к штрихам.
 *
 * Акселерометр меряет сумму гравитации и линейного ускорения и сам их не различает.
 * Штрих по камню даёт 0.1–0.5 g ускорения вдоль камня, и короткое среднее
 * показывает угол с ошибкой 5–25°. Но у штрихов есть два свойства:
 *   1. Нож начинает и кончает штрих в покое, поэтому за несколько штрихов
 *      ускорение в среднем почти нулевое.
 *   2. Ускорение меняется быстро и в одной плоскости (плоскость камня), и разброс
 *      отсчётов в этих направлениях большой. В покое разброс равен шуму, ~1–2 mg.
 *
 * Поэтому три оценки:
 *   fast: экспоненциальное среднее τ = 80 мс: быстрое и точное, пока датчик неподвижен;
 *   slow: два последовательных экспоненциальных средних по τ = 0.8 с: ускорения
 *         штрихов в нём взаимно гасятся (двойное звено гасит их заметно лучше одинарного);
 *   cov:  ковариация отсчётов с забыванием τ = 0.5 с: в каких направлениях и насколько
 *         вектор сейчас «гуляет».
 * Результат: slow + K·(fast − slow), где K = ε²·(cov + ε²·I)⁻¹.
 * В покое cov ≈ 0, K ≈ I, результат равен fast. При движении составляющие разности
 * (fast − slow) в направлениях с разбросом σ² ≫ ε² подавляются в σ²/ε² раз,
 * и результат стремится к slow. Порогов и переключения режимов нет, переход плавный.
 *
 * Цена: быстрый поворот рукой тоже выглядит как движение. После поворота на 10°
 * за 1 с показание догоняет примерно за 2 с (было 0.25 с), после плавной
 * подстройки на 2–3° примерно за 0.5 с.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>

#include "filter.h"

/* α = 1 − exp(−Δt/τ) при Δt = 40 мс (25 Гц). */
#define ALPHA_FAST 0.39f   /* τ = 80 мс */
#define ALPHA_SLOW 0.0488f /* τ = 0.8 с, два звена */
#define ALPHA_COV  0.0769f /* τ = 0.5 с */

/* Порог разброса ε = 30 mg. Шум датчика в покое ~1–2 mg, штрихи дают 50–500 mg. */
#define EPS2 (30.0f * 30.0f)

static bool have_sample;
static float fast[3];
static float slow1[3];
static float slow2[3];
static float cmean[3];
static float cov[6]; /* xx, yy, zz, xy, xz, yz; mg² */

void filter_add(const int16_t mg[3])
{
	const float k = 1.0f - ALPHA_COV;
	float d[3];

	if (!have_sample) {
		for (int a = 0; a < 3; a++) {
			fast[a] = slow1[a] = slow2[a] = cmean[a] = mg[a];
		}
		memset(cov, 0, sizeof(cov));
		have_sample = true;
		return;
	}

	for (int a = 0; a < 3; a++) {
		float v = mg[a];

		fast[a] += ALPHA_FAST * (v - fast[a]);
		slow1[a] += ALPHA_SLOW * (v - slow1[a]);
		slow2[a] += ALPHA_SLOW * (slow1[a] - slow2[a]);
		d[a] = v - cmean[a];
		cmean[a] += ALPHA_COV * d[a];
	}

	/* Ковариация с экспоненциальным окном (аналог алгоритма Уэлфорда). */
	cov[0] = k * (cov[0] + ALPHA_COV * d[0] * d[0]);
	cov[1] = k * (cov[1] + ALPHA_COV * d[1] * d[1]);
	cov[2] = k * (cov[2] + ALPHA_COV * d[2] * d[2]);
	cov[3] = k * (cov[3] + ALPHA_COV * d[0] * d[1]);
	cov[4] = k * (cov[4] + ALPHA_COV * d[0] * d[2]);
	cov[5] = k * (cov[5] + ALPHA_COV * d[1] * d[2]);
}

bool filter_get(float g[3])
{
	/* M = cov + ε²·I: симметричная, собственные числа ≥ ε², поэтому обратима. */
	float a = cov[0] + EPS2, b = cov[1] + EPS2, c = cov[2] + EPS2;
	float d = cov[3], e = cov[4], f = cov[5];
	/* Присоединённая матрица M (тоже симметричная). */
	float m00 = b * c - f * f;
	float m01 = e * f - d * c;
	float m02 = d * f - b * e;
	float m11 = a * c - e * e;
	float m12 = d * e - a * f;
	float m22 = a * b - d * d;
	float det = a * m00 + d * m01 + e * m02;
	float s = EPS2 / det;
	float r[3];

	if (!have_sample) {
		return false;
	}

	for (int i = 0; i < 3; i++) {
		r[i] = fast[i] - slow2[i];
	}
	g[0] = slow2[0] + s * (m00 * r[0] + m01 * r[1] + m02 * r[2]);
	g[1] = slow2[1] + s * (m01 * r[0] + m11 * r[1] + m12 * r[2]);
	g[2] = slow2[2] + s * (m02 * r[0] + m12 * r[1] + m22 * r[2]);

	return true;
}
