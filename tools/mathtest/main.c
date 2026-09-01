/*
 * Host unit tests for the q1psx fixed-point math layer.
 *
 * Compiles engine/q_fixed.h + engine/mathlib.c with the host compiler and
 * compares each op against a double-precision reference. Inputs are quantised
 * to 20.12 first and the reference uses those quantised values, so the tests
 * measure the op's own rounding error, not input quantisation. Run: `make run`.
 */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#include "mathlib.c"        /* unity build: pulls in q_fixed.h + mathlib.h too */
#include "q_trig_table.c"   /* the generated sine table */

static int failures;
static int checks;

/* value a q_fixed literal actually represents, as a double */
static double qd(double x)
{
	return Q_FIX_TO_FLOAT(Q_FIX_FROM_FLOAT(x));
}

/*
 * Assert a 20.12 result matches the reference. Bound = abs_units/4096 (op
 * quantisation floor) + |ref| * rel (chained-multiply rounding).
 */
static void near_rel(q_fixed got, double ref, double abs_units, double rel,
                     const char *msg)
{
	double gf    = Q_FIX_TO_FLOAT(got);
	double d     = fabs(gf - ref);
	double bound = abs_units / (double)Q_FIX_ONE + fabs(ref) * rel;
	checks++;
	if (d > bound) {
		failures++;
		printf("  FAIL  %-30s got %.6f  ref %.6f  (|d|=%.6f > %.6f)\n",
		       msg, gf, ref, d, bound);
	}
}

static void ok(int cond, const char *msg)
{
	checks++;
	if (!cond) {
		failures++;
		printf("  FAIL  %s\n", msg);
	}
}

static double frand(double lo, double hi)
{
	return lo + (hi - lo) * ((double)rand() / (double)RAND_MAX);
}

static void test_scalars(void)
{
	printf("[mathtest] scalars\n");

	ok(Q_FIX(1) == 4096, "Q_FIX(1) == 4096");
	ok(Q_FIX_INT(Q_FIX(7)) == 7, "Q_FIX_INT round-trips");
	ok(Q_FIX_INT(-Q_FIX_ONE / 2) == -1, "Q_FIX_INT floors toward -inf");

	/* products must stay inside the ~+/-524288 representable range */
	for (int i = 0; i < 50000; i++) {
		double a = frand(-600, 600), b = frand(-600, 600);
		near_rel(q_mul(Q_FIX_FROM_FLOAT(a), Q_FIX_FROM_FLOAT(b)),
		         qd(a) * qd(b), 2, 0.0, "q_mul");
		if (fabs(qd(b)) > 0.05)
			near_rel(q_div(Q_FIX_FROM_FLOAT(a), Q_FIX_FROM_FLOAT(b)),
			         qd(a) / qd(b), 2, 3e-4, "q_div");
	}

	for (int i = 0; i < 50000; i++) {
		double x = frand(0, 250000);
		near_rel(q_sqrt_big((uint64_t)Q_FIX_FROM_FLOAT(x)),
		         sqrt(qd(x)), 2, 0.0, "q_sqrt_big");
	}
	near_rel(q_sqrt(Q_FIX(2)), sqrt(2.0), 2, 0.0, "q_sqrt(2)");
	ok(q_sqrt(0) == 0 && q_sqrt(-5) == 0, "q_sqrt clamps <= 0");
}

static void load(vec3_t v, double a, double b, double c)
{
	v[0] = Q_FIX_FROM_FLOAT(a);
	v[1] = Q_FIX_FROM_FLOAT(b);
	v[2] = Q_FIX_FROM_FLOAT(c);
}

static void test_vectors(void)
{
	printf("[mathtest] vectors\n");

	for (int i = 0; i < 50000; i++) {
		/*
		 * Realistic Quake usage: a large vector dotted/crossed with a roughly
		 * unit-scale one (a normal, a direction). Two large vectors would push
		 * the true result past the representable range — that is what
		 * length_sq()/Length() exist for.
		 */
		double ax = frand(-4096, 4096), ay = frand(-4096, 4096), az = frand(-4096, 4096);
		double bx = frand(-4, 4), by = frand(-4, 4), bz = frand(-4, 4);
		double s  = frand(-16, 16);

		vec3_t a, b, out;
		load(a, ax, ay, az);
		load(b, bx, by, bz);

		double qax = qd(ax), qay = qd(ay), qaz = qd(az);
		double qbx = qd(bx), qby = qd(by), qbz = qd(bz), qs = qd(s);

		near_rel(DotProduct(a, b), qax * qbx + qay * qby + qaz * qbz,
		         3, 3e-4, "DotProduct");

		CrossProduct(a, b, out);
		near_rel(out[0], qay * qbz - qaz * qby, 3, 3e-4, "CrossProduct.x");
		near_rel(out[1], qaz * qbx - qax * qbz, 3, 3e-4, "CrossProduct.y");
		near_rel(out[2], qax * qby - qay * qbx, 3, 3e-4, "CrossProduct.z");

		VectorScale(a, Q_FIX_FROM_FLOAT(s), out);
		near_rel(out[0], qax * qs, 3, 3e-4, "VectorScale.x");
		near_rel(out[2], qaz * qs, 3, 3e-4, "VectorScale.z");

		VectorMA(a, Q_FIX_FROM_FLOAT(s), b, out);
		near_rel(out[1], qay + qs * qby, 3, 3e-4, "VectorMA.y");

		double len = sqrt(qax * qax + qay * qay + qaz * qaz);
		near_rel(Length(a), len, 2, 1e-4, "Length");

		vec3_t n;
		VectorCopy(a, n);
		vec_t rl = VectorNormalize(n);
		near_rel(rl, len, 2, 1e-4, "VectorNormalize returns length");
		if (len > 5.0) {
			double nl = sqrt(Q_FIX_TO_FLOAT(n[0]) * Q_FIX_TO_FLOAT(n[0]) +
			                 Q_FIX_TO_FLOAT(n[1]) * Q_FIX_TO_FLOAT(n[1]) +
			                 Q_FIX_TO_FLOAT(n[2]) * Q_FIX_TO_FLOAT(n[2]));
			near_rel(Q_FIX_FROM_FLOAT(nl), 1.0, 4, 1e-3, "normalized is unit length");
		}
	}

	vec3_t p, q;
	load(p, 1.0, 2.0, 3.0);
	load(q, 1.0, 2.0, 3.0);
	ok(VectorCompare(p, q), "VectorCompare equal");
	q[2] += 1;
	ok(!VectorCompare(p, q), "VectorCompare not equal");

	vec3_t iv = { Q_FIX(1), Q_FIX(-2), Q_FIX(3) };
	VectorInverse(iv);
	ok(iv[0] == Q_FIX(-1) && iv[1] == Q_FIX(2) && iv[2] == Q_FIX(-3), "VectorInverse");

	ok(vec3_origin[0] == 0 && vec3_origin[1] == 0 && vec3_origin[2] == 0,
	   "vec3_origin is zero");
}

static void test_trig(void)
{
	printf("[mathtest] trig / angles\n");

	for (int deg = -720; deg <= 720; deg++) {
		double rad = deg * M_PI / 180.0;
		/* table is 1024 steps + 8-bit lerp; worst case a few units off */
		near_rel(q_sin(Q_FIX(deg)), sin(rad), 6, 0.0, "q_sin");
		near_rel(q_cos(Q_FIX(deg)), cos(rad), 6, 0.0, "q_cos");
	}

	ok(anglemod(Q_FIX(370)) == Q_FIX(10), "anglemod 370 -> 10");
	ok(anglemod(Q_FIX(-90)) == Q_FIX(270), "anglemod -90 -> 270");
	ok(anglemod(Q_FIX(0)) == 0 && anglemod(Q_FIX(359)) == Q_FIX(359), "anglemod identity in range");

	/* AngleVectors: forward for yaw=90 is +Y; the basis is orthonormal */
	for (int i = 0; i < 5000; i++) {
		vec3_t ang = { Q_FIX_FROM_FLOAT(frand(-80, 80)),
		               Q_FIX_FROM_FLOAT(frand(-360, 360)),
		               Q_FIX_FROM_FLOAT(frand(-40, 40)) };
		vec3_t f, r, u;
		AngleVectors(ang, f, r, u);

		double pf = Q_FIX_TO_FLOAT(ang[PITCH]) * M_PI / 180.0;
		double yf = Q_FIX_TO_FLOAT(ang[YAW])   * M_PI / 180.0;
		near_rel(f[0], cos(pf) * cos(yf), 8, 0.0, "AngleVectors forward.x");
		near_rel(f[1], cos(pf) * sin(yf), 8, 0.0, "AngleVectors forward.y");
		near_rel(f[2], -sin(pf),          8, 0.0, "AngleVectors forward.z");

		near_rel(DotProduct(f, r), 0.0, 12, 0.0, "forward . right ~ 0");
		near_rel(DotProduct(f, u), 0.0, 12, 0.0, "forward . up ~ 0");
		near_rel(Length(f), 1.0, 6, 2e-3, "|forward| ~ 1");
		near_rel(Length(r), 1.0, 6, 2e-3, "|right| ~ 1");
	}
}

int main(void)
{
	srand(1234);
	test_scalars();
	test_vectors();
	test_trig();

	printf("mathtest: %s  (%d checks, %d failure%s)\n",
	       failures ? "FAIL" : "OK", checks, failures, failures == 1 ? "" : "s");
	return failures ? 1 : 0;
}
