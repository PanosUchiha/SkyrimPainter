// SkyrimPainter - paint your own pictures in Skyrim Special Edition
// Copyright (C) 2026 evodube
//
// This program is free software: you can redistribute it and/or modify it under the terms of the
// GNU General Public License as published by the Free Software Foundation, either version 3 of the
// License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without
// even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
// General Public License for more details.
//
// You should have received a copy of the GNU General Public License along with this program
// (LICENSE.txt). If not, see <https://www.gnu.org/licenses/>.
//
// Port of spectral.js (c) 2025 Ronald van Wijnen, MIT License: https://github.com/rvanwijnen/spectral.js

#include "Spectral.h"

#include "SpectralData.h"

#include <algorithm>
#include <cfloat>
#include <cmath>

namespace Spectral
{
	namespace
	{
		using namespace Data;
		using Vec3 = std::array<double, 3>;

		constexpr double kGamma = 2.4;
		constexpr double kEps = DBL_EPSILON;  // Number.EPSILON

		Vec3 MulMatVec(const double a_m[3][3], const Vec3& a_v)
		{
			return { a_m[0][0] * a_v[0] + a_m[0][1] * a_v[1] + a_m[0][2] * a_v[2],
				a_m[1][0] * a_v[0] + a_m[1][1] * a_v[1] + a_m[1][2] * a_v[2],
				a_m[2][0] * a_v[0] + a_m[2][1] * a_v[1] + a_m[2][2] * a_v[2] };
		}

		double Uncompand(double x) { return x > 0.04045 ? std::pow((x + 0.055) / 1.055, kGamma) : x / 12.92; }
		double Compand(double x) { return x > 0.0031308 ? 1.055 * std::pow(x, 1.0 / kGamma) - 0.055 : x * 12.92; }
		double Clamp(double x, double lo = 0.0, double hi = 1.0) { return std::min(std::max(x, lo), hi); }

		Vec3 SrgbToLinear(const Rgb& c) { return { Uncompand(c[0] / 255.0), Uncompand(c[1] / 255.0), Uncompand(c[2] / 255.0) }; }

		Rgb LinearToSrgb(const Vec3& l)
		{
			Rgb out{};
			for (int i = 0; i < 3; ++i) {
				// spectral.js rounds and then clamps (clip) when the colour is in gamut; we clamp for safety
				out[i] = static_cast<std::uint8_t>(Clamp(std::round(Compand(l[i]) * 255.0), 0.0, 255.0));
			}
			return out;
		}

		struct Color
		{
			double R[kSize];
			double KS[kSize];
			Vec3   XYZ;
			double luminance;
		};

		Vec3 RToXYZ(const double* R)
		{
			Vec3 xyz{};
			for (int row = 0; row < 3; ++row) {
				double s = 0.0;
				for (int i = 0; i < kSize; ++i) {
					s += kCMF[row][i] * R[i];
				}
				xyz[row] = s;
			}
			return xyz;
		}

		void LinearToR(Vec3 l, double* R)
		{
			const double w = std::min({ l[0], l[1], l[2] });
			l = { l[0] - w, l[1] - w, l[2] - w };
			const double c = std::min(l[1], l[2]);
			const double m = std::min(l[0], l[2]);
			const double y = std::min(l[0], l[1]);
			const double r = std::max(0.0, std::min(l[0] - l[2], l[0] - l[1]));
			const double g = std::max(0.0, std::min(l[1] - l[2], l[1] - l[0]));
			const double b = std::max(0.0, std::min(l[2] - l[1], l[2] - l[0]));
			for (int i = 0; i < kSize; ++i) {
				R[i] = std::max(kEps, w * kW[i] + c * kC[i] + m * kM[i] + y * kY[i] + r * kR[i] + g * kG[i] + b * kB[i]);
			}
		}

		double KS(double R) { return (1.0 - R) * (1.0 - R) / (2.0 * R); }
		double KM(double ks) { return 1.0 + ks - std::sqrt(ks * ks + 2.0 * ks); }

		Color FromSrgb(const Rgb& a_c)
		{
			Color c{};
			LinearToR(SrgbToLinear(a_c), c.R);
			for (int i = 0; i < kSize; ++i) {
				c.KS[i] = KS(c.R[i]);
			}
			c.XYZ = RToXYZ(c.R);
			c.luminance = std::max(kEps, c.XYZ[1]);
			return c;
		}

		// ---- gamut mapping (spectral.js gamutMap: reduce OKLCh chroma until in gamut)
		Vec3 XYZToOKLab(const Vec3& xyz)
		{
			Vec3 lms = MulMatVec(kXYZ_LMS, xyz);
			for (auto& v : lms) {
				v = std::cbrt(v);
			}
			return MulMatVec(kLMS_LAB, lms);
		}

		Vec3 OKLabToXYZ(const Vec3& lab)
		{
			Vec3 lms = MulMatVec(kLAB_LMS, lab);
			for (auto& v : lms) {
				v = v * v * v;
			}
			return MulMatVec(kLMS_XYZ, lms);
		}

		bool InGamut(const Vec3& l) { return std::all_of(l.begin(), l.end(), [](double x) { return x >= 0.0 && x <= 1.0; }); }

		double DeltaEOK(const Vec3& a, const Vec3& b)
		{
			return std::sqrt((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2]));
		}

		Vec3 LinearToOKLab(const Vec3& l) { return XYZToOKLab(MulMatVec(kRGB_XYZ, l)); }

		Rgb GamutMap(const Vec3& a_xyz)
		{
			constexpr double jnd = 0.03, e = 0.0001;
			const Vec3       lab = XYZToOKLab(a_xyz);
			const double     L = lab[0];
			const double     C = std::sqrt(lab[1] * lab[1] + lab[2] * lab[2]);
			double           h = std::atan2(lab[2], lab[1]) * 180.0 / 3.14159265358979323846;
			if (h < 0) {
				h += 360.0;
			}
			if (L >= 1.0) {
				return { 255, 255, 255 };
			}
			if (L <= 0.0) {
				return { 0, 0, 0 };
			}
			Vec3 current = MulMatVec(kXYZ_RGB, a_xyz);
			if (InGamut(current)) {
				return LinearToSrgb(current);
			}
			double min = 0.0, max = C;
			bool   minInGamut = true;
			auto   clip = [](Vec3 v) {
                for (auto& x : v) {
                    x = Clamp(x);
                }
                return v;
			};
			Vec3   clipped = LinearToOKLab(clip(current));
			double E = DeltaEOK(clipped, LinearToOKLab(current));
			if (E < jnd) {
				return LinearToSrgb(MulMatVec(kXYZ_RGB, OKLabToXYZ(clipped)));
			}
			while (max - min > e) {
				const double chroma = (min + max) / 2.0;
				const Vec3   ok{ L, chroma * std::cos(h * 3.14159265358979323846 / 180.0), chroma * std::sin(h * 3.14159265358979323846 / 180.0) };
				current = MulMatVec(kXYZ_RGB, OKLabToXYZ(ok));
				if (minInGamut && InGamut(current)) {
					min = chroma;
				} else {
					clipped = LinearToOKLab(clip(current));
					E = DeltaEOK(clipped, ok);
					if (E < jnd) {
						if (jnd - E < e) {
							break;
						}
						minInGamut = false;
						min = chroma;
					} else {
						max = chroma;
					}
				}
			}
			return LinearToSrgb(MulMatVec(kXYZ_RGB, OKLabToXYZ(clipped)));
		}
	}

	Rgb Mix(const std::vector<std::pair<Rgb, double>>& a_colors)
	{
		if (a_colors.empty()) {
			return { 0, 0, 0 };
		}
		std::vector<Color> cols;
		cols.reserve(a_colors.size());
		for (const auto& [rgb, f] : a_colors) {
			cols.push_back(FromSrgb(rgb));
		}
		double R[kSize];
		for (int i = 0; i < kSize; ++i) {
			double ksMix = 0.0, total = 0.0;
			for (std::size_t k = 0; k < cols.size(); ++k) {
				const double factor = a_colors[k].second;
				const double conc = factor * factor * cols[k].luminance;  // tinting strength 1
				total += conc;
				ksMix += cols[k].KS[i] * conc;
			}
			R[i] = KM(total > 0.0 ? ksMix / total : 0.0);
		}
		return GamutMap(RToXYZ(R));
	}

	Rgb Mix(const Rgb& a_a, const Rgb& a_b, double a_t)
	{
		const double t = Clamp(a_t);
		return Mix({ { a_a, 1.0 - t }, { a_b, t } });
	}
}
