/*
This file is part of MoltenGram Desktop,
based on Telegram Desktop and exteraGram.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "extera/molten_qr.h"

#include "extera/extera_config.h"
#include "lang/lang_keys.h"
#include "styles/style_boxes.h"
#include "styles/style_layers.h"
#include "ui/rp_widget.h"
#include "ui/layers/generic_box.h"
#include "ui/painter.h"
#include "ui/toast/toast.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/labels.h"
#include "ui/wrap/vertical_layout.h"
#include "window/window_session_controller.h"
#include "main/main_session.h"
#include "data/data_user.h"

#if __has_include("qr/qr_generate.h")
#include "qr/qr_generate.h"
#define MOLTEN_HAS_LIB_QR 1
#else
#define MOLTEN_HAS_LIB_QR 0
#endif

#include <QtCore/QPointF>
#include <QtCore/QMimeData>
#include <QtGui/QGuiApplication>
#include <QtGui/QClipboard>
#include <QtGui/QPainter>
#include <QtGui/QPainterPath>
#include <QtGui/QLinearGradient>
#include <QtGui/QMouseEvent>
#include <QtWidgets/QFileDialog>
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace Extera::QrGenerator {
namespace {

const auto kConfigKeyDotStyle = u"qr_dot_style"_q;
const auto kConfigKeyMd3Card = u"qr_md3_card"_q;
const auto kConfigKeyMoltenLogo = u"qr_molten_logo"_q;

// Internal Nayuki QR Code Generator implementation for self-contained fallback
namespace internal_qr {

class BitBuffer final : public std::vector<bool> {
public:
	BitBuffer() = default;

	void appendBits(std::uint32_t val, int len) {
		if (len < 0 || len > 31 || (len < 31 && (val >> len) != 0)) {
			throw std::domain_error("Value out of range");
		}
		for (int i = len - 1; i >= 0; --i) {
			this->push_back(((val >> i) & 1) != 0);
		}
	}
};

class QrSegment final {
public:
	enum class Mode {
		NUMERIC,
		ALPHANUMERIC,
		BYTE,
		KANJI,
		ECI,
	};

	static QrSegment makeBytes(const std::vector<std::uint8_t> &data) {
		BitBuffer bb;
		for (std::uint8_t b : data) {
			bb.appendBits(b, 8);
		}
		return QrSegment(Mode::BYTE, static_cast<int>(data.size()), std::move(bb));
	}

	static QrSegment makeBytes(const char *dataStr) {
		std::vector<std::uint8_t> bytes;
		if (dataStr) {
			for (const char *p = dataStr; *p != '\0'; ++p) {
				bytes.push_back(static_cast<std::uint8_t>(*p));
			}
		}
		return makeBytes(bytes);
	}

	QrSegment(Mode md, int numCh, BitBuffer &&bData)
	: _mode(md)
	, _numChars(numCh)
	, _data(std::move(bData)) {
		if (numCh < 0) {
			throw std::domain_error("Invalid character count");
		}
	}

	[[nodiscard]] Mode getMode() const { return _mode; }
	[[nodiscard]] int getNumChars() const { return _numChars; }
	[[nodiscard]] const BitBuffer &getData() const { return _data; }

	static int getTotalBits(const std::vector<QrSegment> &segs, int version) {
		int result = 0;
		for (const auto &seg : segs) {
			const int ccbits = numCharCountBits(seg.getMode(), version);
			if (seg.getNumChars() >= (1L << ccbits)) {
				return -1;
			}
			if (4 + ccbits > 100000000 - result) {
				return -1;
			}
			result += 4 + ccbits;
			if (static_cast<int>(seg.getData().size()) > 100000000 - result) {
				return -1;
			}
			result += static_cast<int>(seg.getData().size());
		}
		return result;
	}

	static int numCharCountBits(Mode mode, int version) {
		const int i = (version + 7) / 17;
		switch (mode) {
		case Mode::NUMERIC: {
			static constexpr int kNumBits[] = { 10, 12, 14 };
			return kNumBits[i];
		}
		case Mode::ALPHANUMERIC: {
			static constexpr int kAlphaBits[] = { 9, 11, 13 };
			return kAlphaBits[i];
		}
		case Mode::BYTE: {
			static constexpr int kByteBits[] = { 8, 16, 16 };
			return kByteBits[i];
		}
		case Mode::KANJI: {
			static constexpr int kKanjiBits[] = { 8, 10, 12 };
			return kKanjiBits[i];
		}
		case Mode::ECI:
			return 0;
		}
		return 0;
	}

private:
	Mode _mode;
	int _numChars;
	BitBuffer _data;
};

class QrCode final {
public:
	enum class Ecc {
		LOW = 0,
		MEDIUM = 1,
		QUARTILE = 2,
		HIGH = 3,
	};

	static constexpr int kMinVersion = 1;
	static constexpr int kMaxVersion = 40;

	static QrCode encodeText(const char *text, Ecc ecc) {
		std::vector<QrSegment> segs;
		segs.push_back(QrSegment::makeBytes(text));
		return encodeSegments(segs, ecc);
	}

	static QrCode encodeSegments(
			const std::vector<QrSegment> &segs,
			Ecc ecc,
			int minVersion = 1,
			int maxVersion = 40,
			int mask = -1,
			bool boostEcl = true) {
		if (!(kMinVersion <= minVersion && minVersion <= maxVersion && maxVersion <= kMaxVersion)
			|| mask < -1 || mask > 7) {
			throw std::invalid_argument("Invalid arguments");
		}

		int version = minVersion;
		int dataUsedBits = 0;
		for (;; ++version) {
			const int dataCapBits = getNumDataCodewords(version, ecc) * 8;
			const int usedBits = QrSegment::getTotalBits(segs, version);
			if (usedBits != -1 && usedBits <= dataCapBits) {
				dataUsedBits = usedBits;
				break;
			}
			if (version >= maxVersion) {
				throw std::length_error("Data does not fit in given versions");
			}
		}

		if (boostEcl) {
			static constexpr Ecc kEccList[] = { Ecc::MEDIUM, Ecc::QUARTILE, Ecc::HIGH };
			for (Ecc newEcc : kEccList) {
				if (static_cast<int>(newEcc) > static_cast<int>(ecc)
					&& dataUsedBits <= getNumDataCodewords(version, newEcc) * 8) {
					ecc = newEcc;
				}
			}
		}

		BitBuffer bb;
		for (const auto &seg : segs) {
			bb.appendBits(getModeBits(seg.getMode()), 4);
			bb.appendBits(static_cast<std::uint32_t>(seg.getNumChars()), QrSegment::numCharCountBits(seg.getMode(), version));
			for (bool b : seg.getData()) {
				bb.push_back(b);
			}
		}

		const int dataCapBits = getNumDataCodewords(version, ecc) * 8;
		const int terminatorBits = std::min(4, dataCapBits - static_cast<int>(bb.size()));
		bb.appendBits(0, terminatorBits);
		const int paddingBits = (8 - (static_cast<int>(bb.size()) % 8)) % 8;
		bb.appendBits(0, paddingBits);

		for (std::uint8_t padByte = 0xEC; bb.size() < static_cast<std::size_t>(dataCapBits); padByte ^= 0xEC ^ 0x11) {
			bb.appendBits(padByte, 8);
		}

		std::vector<std::uint8_t> dataCodewords(bb.size() / 8);
		for (std::size_t i = 0; i < bb.size(); ++i) {
			dataCodewords[i >> 3] = static_cast<std::uint8_t>(
				dataCodewords[i >> 3] | (bb[i] ? (1 << (7 - (i & 7))) : 0));
		}

		return QrCode(version, ecc, dataCodewords, mask);
	}

	QrCode(int ver, Ecc ecl, const std::vector<std::uint8_t> &dataCodewords, int msk)
	: _version(ver)
	, _errorCorrectionLevel(ecl)
	, _size(ver * 4 + 17)
	, _modules(_size, std::vector<bool>(_size, false))
	, _isFunction(_size, std::vector<bool>(_size, false)) {
		drawFunctionPatterns();
		const auto allCodewords = addErrorCorrection(dataCodewords);
		drawCodewords(allCodewords);

		if (msk == -1) {
			long minPenalty = 1000000000L;
			for (int i = 0; i < 8; ++i) {
				applyMask(i);
				drawFormatBits(i);
				const long penalty = getPenaltyScore();
				if (penalty < minPenalty) {
					msk = i;
					minPenalty = penalty;
				}
				applyMask(i);
			}
		}
		_mask = msk;
		applyMask(msk);
		drawFormatBits(msk);
	}

	[[nodiscard]] int getSize() const { return _size; }
	[[nodiscard]] bool getModule(int x, int y) const {
		return (0 <= x && x < _size && 0 <= y && y < _size) ? _modules[y][x] : false;
	}

private:
	static std::uint32_t getModeBits(QrSegment::Mode mode) {
		switch (mode) {
		case QrSegment::Mode::NUMERIC: return 0x1;
		case QrSegment::Mode::ALPHANUMERIC: return 0x2;
		case QrSegment::Mode::BYTE: return 0x4;
		case QrSegment::Mode::KANJI: return 0x8;
		case QrSegment::Mode::ECI: return 0x7;
		}
		return 0;
	}

	static int getNumDataCodewords(int ver, Ecc ecl) {
		return getNumRawDataModules(ver) / 8 - getEccCodewordsPerBlock(ver, ecl) * numErrorCorrectionBlocks(ver, ecl);
	}

	static int getNumRawDataModules(int ver) {
		int result = (16 * ver + 128) * ver + 64;
		if (ver >= 2) {
			const int numAlign = ver / 7 + 2;
			result -= (25 * numAlign - 10) * numAlign - 55;
			if (ver >= 7) {
				result -= 36;
			}
		}
		return result;
	}

	static int numErrorCorrectionBlocks(int ver, Ecc ecl) {
		static constexpr std::int8_t kEccBlocks[4][40] = {
			// Low
			{ 1, 1, 1, 1, 1, 2, 2, 2, 2, 4, 4, 4, 4, 4, 6, 6, 6, 6, 7, 8, 8, 9, 9, 10, 12, 12, 12, 13, 14, 15, 16, 17, 18, 19, 19, 20, 21, 22, 24, 25 },
			// Medium
			{ 1, 1, 1, 2, 2, 4, 4, 4, 5, 5, 5, 8, 9, 9, 10, 10, 11, 13, 14, 16, 17, 17, 18, 20, 21, 23, 25, 26, 28, 29, 31, 33, 35, 37, 38, 40, 43, 45, 47, 49 },
			// Quartile
			{ 1, 1, 2, 2, 4, 4, 6, 6, 8, 8, 8, 10, 12, 16, 12, 17, 16, 18, 21, 20, 23, 23, 25, 27, 29, 34, 34, 35, 38, 40, 43, 45, 48, 51, 53, 56, 59, 62, 65, 68 },
			// High
			{ 1, 1, 2, 4, 4, 4, 5, 6, 8, 8, 11, 11, 16, 16, 18, 16, 19, 21, 25, 25, 25, 34, 30, 32, 35, 37, 40, 42, 45, 48, 51, 54, 57, 60, 63, 66, 70, 72, 74, 79 },
		};
		return kEccBlocks[static_cast<int>(ecl)][ver - 1];
	}

	static int getEccCodewordsPerBlock(int ver, Ecc ecl) {
		static constexpr std::int8_t kEccCodewords[4][40] = {
			// Low
			{ 7, 10, 15, 20, 26, 18, 20, 24, 30, 18, 20, 24, 26, 30, 22, 24, 28, 30, 28, 28, 28, 28, 30, 30, 26, 28, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30 },
			// Medium
			{ 10, 16, 26, 18, 24, 16, 18, 22, 22, 26, 30, 22, 22, 24, 24, 28, 28, 26, 26, 26, 26, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28 },
			// Quartile
			{ 13, 22, 18, 26, 18, 24, 18, 22, 20, 24, 28, 26, 24, 20, 30, 24, 28, 28, 26, 30, 28, 30, 30, 30, 30, 28, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30 },
			// High
			{ 17, 28, 22, 16, 22, 28, 26, 26, 24, 28, 24, 28, 22, 24, 24, 30, 28, 28, 26, 28, 30, 24, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30 },
		};
		return kEccCodewords[static_cast<int>(ecl)][ver - 1];
	}

	void drawFunctionPatterns() {
		for (int i = 0; i < _size; ++i) {
			setFunctionModule(6, i, i % 2 == 0);
			setFunctionModule(i, 6, i % 2 == 0);
		}
		drawFinderPattern(3, 3);
		drawFinderPattern(_size - 4, 3);
		drawFinderPattern(3, _size - 4);

		const auto alignPos = getAlignmentPatternPositions();
		const std::size_t numAlign = alignPos.size();
		for (std::size_t i = 0; i < numAlign; ++i) {
			for (std::size_t j = 0; j < numAlign; ++j) {
				if (!((i == 0 && j == 0) || (i == 0 && j == numAlign - 1) || (i == numAlign - 1 && j == 0))) {
					drawAlignmentPattern(alignPos[i], alignPos[j]);
				}
			}
		}

		drawFormatBits(0);
		drawVersion();
	}

	void drawFinderPattern(int x, int y) {
		for (int dy = -4; dy <= 4; ++dy) {
			for (int dx = -4; dx <= 4; ++dx) {
				const int dist = std::max(std::abs(dx), std::abs(dy));
				const int xx = x + dx;
				const int yy = y + dy;
				if (0 <= xx && xx < _size && 0 <= yy && yy < _size) {
					setFunctionModule(xx, yy, dist != 2 && dist != 4);
				}
			}
		}
	}

	void drawAlignmentPattern(int x, int y) {
		for (int dy = -2; dy <= 2; ++dy) {
			for (int dx = -2; dx <= 2; ++dx) {
				setFunctionModule(x + dx, y + dy, std::max(std::abs(dx), std::abs(dy)) != 1);
			}
		}
	}

	void setFunctionModule(int x, int y, bool isDark) {
		_modules[y][x] = isDark;
		_isFunction[y][x] = true;
	}

	void drawFormatBits(int msk) {
		static constexpr int kFormatBits[4] = { 1, 0, 3, 2 };
		int data = kFormatBits[static_cast<int>(_errorCorrectionLevel)] << 3 | msk;
		int rem = data;
		for (int i = 0; i < 10; ++i) {
			rem = (rem << 1) ^ ((rem >> 9) * 0x537);
		}
		int bits = (data << 10 | rem) ^ 0x5412;

		for (int i = 0; i <= 5; ++i) {
			setFunctionModule(8, i, ((bits >> i) & 1) != 0);
		}
		setFunctionModule(8, 7, ((bits >> 6) & 1) != 0);
		setFunctionModule(8, 8, ((bits >> 7) & 1) != 0);
		setFunctionModule(7, 8, ((bits >> 8) & 1) != 0);
		for (int i = 9; i < 15; ++i) {
			setFunctionModule(14 - i, 8, ((bits >> i) & 1) != 0);
		}

		for (int i = 0; i < 8; ++i) {
			setFunctionModule(_size - 1 - i, 8, ((bits >> i) & 1) != 0);
		}
		for (int i = 8; i < 15; ++i) {
			setFunctionModule(8, _size - 15 + i, ((bits >> i) & 1) != 0);
		}
		setFunctionModule(8, _size - 8, true);
	}

	void drawVersion() {
		if (_version < 7) {
			return;
		}
		int rem = _version;
		for (int i = 0; i < 12; ++i) {
			rem = (rem << 1) ^ ((rem >> 11) * 0x1F25);
		}
		const long bits = static_cast<long>(_version) << 12 | rem;
		for (int i = 0; i < 18; ++i) {
			const bool a = ((bits >> i) & 1) != 0;
			const int b = _size - 11 + i % 3;
			const int c = i / 3;
			setFunctionModule(b, c, a);
			setFunctionModule(c, b, a);
		}
	}

	[[nodiscard]] std::vector<int> getAlignmentPatternPositions() const {
		if (_version == 1) {
			return {};
		}
		const int numAlign = _version / 7 + 2;
		const int step = (_version == 32) ? 26 : ((_version * 4 + numAlign * 2 + 1) / (numAlign * 2 - 2) * 2);
		std::vector<int> result(numAlign);
		result[0] = 6;
		for (int i = numAlign - 1, pos = _size - 7; i >= 1; --i, pos -= step) {
			result[i] = pos;
		}
		return result;
	}

	[[nodiscard]] std::vector<std::uint8_t> addErrorCorrection(const std::vector<std::uint8_t> &data) const {
		const int numBlocks = numErrorCorrectionBlocks(_version, _errorCorrectionLevel);
		const int blockEccLen = getEccCodewordsPerBlock(_version, _errorCorrectionLevel);
		const int rawCodewords = getNumRawDataModules(_version) / 8;
		const int numShortBlocks = numBlocks - rawCodewords % numBlocks;
		const int shortBlockLen = rawCodewords / numBlocks;

		std::vector<std::vector<std::uint8_t>> blocks;
		const auto rsPoly = reedSolomonComputeDivisor(blockEccLen);
		for (int i = 0, k = 0; i < numBlocks; ++i) {
			const int curLen = shortBlockLen - blockEccLen + (i >= numShortBlocks ? 1 : 0);
			std::vector<std::uint8_t> dat(&data[k], &data[k + curLen]);
			k += curLen;
			const auto ecc = reedSolomonComputeRemainder(dat, rsPoly);
			dat.insert(dat.end(), ecc.begin(), ecc.end());
			blocks.push_back(std::move(dat));
		}

		std::vector<std::uint8_t> result;
		for (std::size_t i = 0; i < blocks[0].size(); ++i) {
			for (std::size_t j = 0; j < blocks.size(); ++j) {
				if (i < blocks[j].size()) {
					result.push_back(blocks[j][i]);
				}
			}
		}
		return result;
	}

	static std::vector<std::uint8_t> reedSolomonComputeDivisor(int degree) {
		std::vector<std::uint8_t> result(degree, 0);
		result[degree - 1] = 1;
		std::uint8_t root = 1;
		for (int i = 0; i < degree; ++i) {
			for (std::size_t j = 0; j < result.size(); ++j) {
				result[j] = reedSolomonMultiply(result[j], root);
				if (j + 1 < result.size()) {
					result[j] ^= result[j + 1];
				}
			}
			root = reedSolomonMultiply(root, 0x02);
		}
		return result;
	}

	static std::vector<std::uint8_t> reedSolomonComputeRemainder(
			const std::vector<std::uint8_t> &data,
			const std::vector<std::uint8_t> &divisor) {
		std::vector<std::uint8_t> result(divisor.size(), 0);
		for (std::uint8_t b : data) {
			const std::uint8_t factor = b ^ result[0];
			result.erase(result.begin());
			result.push_back(0);
			for (std::size_t i = 0; i < divisor.size(); ++i) {
				result[i] ^= reedSolomonMultiply(divisor[i], factor);
			}
		}
		return result;
	}

	static std::uint8_t reedSolomonMultiply(std::uint8_t x, std::uint8_t y) {
		int z = 0;
		for (int i = 7; i >= 0; --i) {
			z = (z << 1) ^ ((z >> 8) * 0x11D);
			z ^= ((y >> i) & 1) * x;
		}
		return static_cast<std::uint8_t>(z);
	}

	void drawCodewords(const std::vector<std::uint8_t> &data) {
		std::size_t i = 0;
		for (int right = _size - 1; right >= 1; right -= 2) {
			if (right == 6) {
				right = 5;
			}
			for (int vert = 0; vert < _size; ++vert) {
				for (int j = 0; j < 2; ++j) {
					const int x = right - j;
					const bool upward = ((right + 1) & 2) == 0;
					const int y = upward ? _size - 1 - vert : vert;
					if (!_isFunction[y][x] && i < data.size() * 8) {
						_modules[y][x] = ((data[i >> 3] >> (7 - (i & 7))) & 1) != 0;
						++i;
					}
				}
			}
		}
	}

	void applyMask(int msk) {
		for (int y = 0; y < _size; ++y) {
			for (int x = 0; x < _size; ++x) {
				bool invert = false;
				switch (msk) {
				case 0: invert = (x + y) % 2 == 0; break;
				case 1: invert = y % 2 == 0; break;
				case 2: invert = x % 3 == 0; break;
				case 3: invert = (x + y) % 3 == 0; break;
				case 4: invert = (x / 3 + y / 2) % 2 == 0; break;
				case 5: invert = (x * y) % 2 + (x * y) % 3 == 0; break;
				case 6: invert = ((x * y) % 2 + (x * y) % 3) % 2 == 0; break;
				case 7: invert = ((x + y) % 2 + (x * y) % 3) % 2 == 0; break;
				default: break;
				}
				_modules[y][x] = _modules[y][x] ^ (invert && !_isFunction[y][x]);
			}
		}
	}

	[[nodiscard]] long getPenaltyScore() const {
		long result = 0;
		for (int y = 0; y < _size; ++y) {
			bool runColor = false;
			int runX = 0;
			std::array<int, 7> runHistory = {};
			for (int x = 0; x < _size; ++x) {
				if (_modules[y][x] == runColor) {
					++runX;
					if (runX == 5) {
						result += 3;
					} else if (runX > 5) {
						result += 1;
					}
				} else {
					finderPenaltyAddHistory(runX, runHistory);
					if (!runColor && finderPenaltyCountPatterns(runHistory)) {
						result += 40;
					}
					runColor = _modules[y][x];
					runX = 1;
				}
			}
			finderPenaltyAddHistory(runX, runHistory);
			if (runColor) {
				finderPenaltyAddHistory(0, runHistory);
			}
			if (finderPenaltyCountPatterns(runHistory)) {
				result += 40;
			}
		}

		for (int x = 0; x < _size; ++x) {
			bool runColor = false;
			int runY = 0;
			std::array<int, 7> runHistory = {};
			for (int y = 0; y < _size; ++y) {
				if (_modules[y][x] == runColor) {
					++runY;
					if (runY == 5) {
						result += 3;
					} else if (runY > 5) {
						result += 1;
					}
				} else {
					finderPenaltyAddHistory(runY, runHistory);
					if (!runColor && finderPenaltyCountPatterns(runHistory)) {
						result += 40;
					}
					runColor = _modules[y][x];
					runY = 1;
				}
			}
			finderPenaltyAddHistory(runY, runHistory);
			if (runColor) {
				finderPenaltyAddHistory(0, runHistory);
			}
			if (finderPenaltyCountPatterns(runHistory)) {
				result += 40;
			}
		}

		for (int y = 0; y < _size - 1; ++y) {
			for (int x = 0; x < _size - 1; ++x) {
				const bool color = _modules[y][x];
				if (color == _modules[y][x + 1]
					&& color == _modules[y + 1][x]
					&& color == _modules[y + 1][x + 1]) {
					result += 3;
				}
			}
		}

		int total = 0;
		for (const auto &row : _modules) {
			for (bool color : row) {
				if (color) {
					++total;
				}
			}
		}
		const int totalModules = _size * _size;
		const int k = static_cast<int>((std::abs(total * 20L - totalModules * 10L) + totalModules - 1) / totalModules) - 1;
		result += k * 10;
		return result;
	}

	static void finderPenaltyAddHistory(int currentRunLength, std::array<int, 7> &runHistory) {
		if (runHistory[0] == 0) {
			currentRunLength += 0;
		}
		std::copy_backward(runHistory.cbegin(), runHistory.cend() - 1, runHistory.end());
		runHistory[0] = currentRunLength;
	}

	static bool finderPenaltyCountPatterns(const std::array<int, 7> &runHistory) {
		const int n = runHistory[1];
		return n > 0
			&& runHistory[2] == n
			&& runHistory[3] == n * 3
			&& runHistory[4] == n
			&& runHistory[5] == n
			&& (runHistory[0] >= n * 4 || runHistory[6] >= n * 4);
	}

	int _version;
	Ecc _errorCorrectionLevel;
	int _size;
	int _mask = 0;
	std::vector<std::vector<bool>> _modules;
	std::vector<std::vector<bool>> _isFunction;
};

} // namespace internal_qr

// Custom QWidget for rendering interactive live QR Card in dialog
class QrCardWidget final : public Ui::RpWidget {
public:
	QrCardWidget(
		QWidget *parent,
		const QString &link,
		const RenderOptions &options)
	: RpWidget(parent)
	, _link(link)
	, _options(options)
	, _matrix(GenerateMatrix(link)) {
		setMouseTracking(true);
		resize(340, 440);
	}

	void setDotStyle(DotStyle style) {
		_options.dotStyle = style;
		SetCurrentDotStyle(style);
		update();
	}

	[[nodiscard]] RenderOptions currentOptions() const {
		return _options;
	}

protected:
	void paintEvent(QPaintEvent *e) override {
		QPainter p(this);
		PaintCard(p, rect(), _matrix, _options);

		// Draw style switch chips near the bottom
		paintStyleChips(p);
	}

	void mousePressEvent(QMouseEvent *e) override {
		const auto chip = chipAt(e->pos());
		if (chip >= 0 && chip < static_cast<int>(AvailableDotStyles().size())) {
			setDotStyle(AvailableDotStyles()[chip]);
		}
	}

	void mouseMoveEvent(QMouseEvent *e) override {
		const auto chip = chipAt(e->pos());
		setCursor(chip >= 0 ? Qt::PointingHandCursor : Qt::ArrowCursor);
	}

	QSize sizeHint() const override {
		return QSize(340, 440);
	}

	QSize minimumSizeHint() const override {
		return QSize(300, 410);
	}

private:
	void paintStyleChips(QPainter &p) {
		PainterHighQualityEnabler hq(p);
		const auto &styles = AvailableDotStyles();
		const auto count = static_cast<int>(styles.size());
		const int chipHeight = 24;
		const int chipY = height() - chipHeight - 16;
		const int totalW = width() - 32;
		const int chipW = std::clamp(totalW / count - 4, 38, 56);
		const int startX = (width() - (count * chipW + (count - 1) * 4)) / 2;

		auto font = p.font();
		font.setPixelSize(10);
		font.setBold(true);
		p.setFont(font);

		for (int i = 0; i < count; ++i) {
			const auto rect = QRect(startX + i * (chipW + 4), chipY, chipW, chipHeight);
			const bool active = (styles[i] == _options.dotStyle);

			if (active) {
				p.setPen(Qt::NoPen);
				p.setBrush(_options.accentColor);
				p.drawRoundedRect(rect, 12, 12);
				p.setPen(Qt::white);
			} else {
				p.setPen(QPen(QColor(0xDC, 0xDC, 0xE2), 1.0));
				p.setBrush(QColor(0xF4, 0xF4, 0xF8));
				p.drawRoundedRect(rect, 12, 12);
				p.setPen(QColor(0x4E, 0x4E, 0x56));
			}

			const auto name = DotStyleName(styles[i]);
			p.drawText(rect, Qt::AlignCenter, name);
		}
	}

	[[nodiscard]] int chipAt(QPoint pos) const {
		const auto &styles = AvailableDotStyles();
		const auto count = static_cast<int>(styles.size());
		const int chipHeight = 24;
		const int chipY = height() - chipHeight - 16;
		const int totalW = width() - 32;
		const int chipW = std::clamp(totalW / count - 4, 38, 56);
		const int startX = (width() - (count * chipW + (count - 1) * 4)) / 2;

		for (int i = 0; i < count; ++i) {
			const auto rect = QRect(startX + i * (chipW + 4), chipY, chipW, chipHeight);
			if (rect.contains(pos)) {
				return i;
			}
		}
		return -1;
	}

	QString _link;
	RenderOptions _options;
	Matrix _matrix;
};

} // namespace

Matrix GenerateMatrix(const QString &text) {
	if (text.isEmpty()) {
		return {};
	}
#if MOLTEN_HAS_LIB_QR
	const auto data = Qr::Encode(text, Qr::Redundancy::Quartile);
	if (data.size > 0 && !data.values.empty()) {
		Matrix matrix;
		matrix.size = data.size;
		matrix.modules.resize(data.size * data.size);
		for (int y = 0; y < data.size; ++y) {
			for (int x = 0; x < data.size; ++x) {
				matrix.modules[y * data.size + x] = data.values[y * data.size + x] ? 1 : 0;
			}
		}
		return matrix;
	}
#endif
	try {
		const auto qr = internal_qr::QrCode::encodeText(
			text.toUtf8().constData(),
			internal_qr::QrCode::Ecc::QUARTILE);
		const auto size = qr.getSize();
		Matrix matrix;
		matrix.size = size;
		matrix.modules.resize(size * size);
		for (int y = 0; y < size; ++y) {
			for (int x = 0; x < size; ++x) {
				matrix.modules[y * size + x] = qr.getModule(x, y) ? 1 : 0;
			}
		}
		return matrix;
	} catch (...) {
		return {};
	}
}

void PaintLogo(
		QPainter &p,
		const QRectF &logoRect,
		const QColor &accentColor) {
	PainterHighQualityEnabler hq(p);
	const auto c = logoRect.center();
	const auto r = logoRect.width() * 0.46;

	// Vibrant Molten flame / star gradient
	auto grad = QLinearGradient(c.x() - r, c.y() - r, c.x() + r, c.y() + r);
	grad.setColorAt(0.0, QColor(0xFF, 0x45, 0x00)); // Molten Orange-Red
	grad.setColorAt(0.5, QColor(0xFF, 0x70, 0x43)); // Coral flame
	grad.setColorAt(1.0, QColor(0x7E, 0x57, 0xC2)); // MoltenStar Violet
	p.setPen(Qt::NoPen);
	p.setBrush(grad);
	p.drawEllipse(c, r, r);

	// Try loading embedded star icon if present in resource pack
	static const auto kStarIcon = QImage(u":/gui/art/molten_icons/star.png"_q);
	if (!kStarIcon.isNull()) {
		const auto innerR = r * 0.72;
		const auto iconRect = QRectF(c.x() - innerR, c.y() - innerR, 2 * innerR, 2 * innerR);
		p.drawImage(iconRect.toRect(), kStarIcon);
	} else {
		// Vector 4-point astroid star glyph with flared curves
		auto star = QPainterPath();
		const auto pinch = r * 0.22;
		star.moveTo(c.x(), c.y() - r * 0.72);
		star.quadTo(c.x() + pinch, c.y() - pinch, c.x() + r * 0.72, c.y());
		star.quadTo(c.x() + pinch, c.y() + pinch, c.x(), c.y() + r * 0.72);
		star.quadTo(c.x() - pinch, c.y() + pinch, c.x() - r * 0.72, c.y());
		star.quadTo(c.x() - pinch, c.y() - pinch, c.x(), c.y() - r * 0.72);
		star.closeSubpath();
		p.setBrush(Qt::white);
		p.setPen(Qt::NoPen);
		p.drawPath(star);

		// Center golden core spark
		p.setBrush(QColor(0xFF, 0xEE, 0x58));
		p.drawEllipse(c, r * 0.16, r * 0.16);
	}
}

void PaintQr(
		QPainter &p,
		const QRect &qrRect,
		const Matrix &matrix,
		const RenderOptions &options) {
	PainterHighQualityEnabler hq(p);
	const int n = matrix.size;
	if (n <= 0) {
		return;
	}

	const double cell = qrRect.width() / static_cast<double>(n);
	const double qrLeft = qrRect.left();
	const double qrTop = qrRect.top();
	const auto center = qrRect.center();

	const double cutoutRadius = (options.logoCutoutRadius > 0)
		? options.logoCutoutRadius
		: (qrRect.width() * 0.145);
	const double cutoutRadiusSq = (cutoutRadius + cell * 0.45) * (cutoutRadius + cell * 0.45);

	// Finder pattern geometries
	const auto drawFinder = [&](double fx, double fy) {
		const auto finderRect = QRectF(fx, fy, 7 * cell, 7 * cell);
		double outerRadius = 0.0;
		if (options.dotStyle == DotStyle::Dots) {
			outerRadius = 7 * cell * 0.48;
		} else if (options.dotStyle == DotStyle::Squircles) {
			outerRadius = 7 * cell * 0.32;
		} else if (options.dotStyle == DotStyle::RoundedSquares) {
			outerRadius = 7 * cell * 0.22;
		} else if (options.dotStyle == DotStyle::Md3Shapes) {
			outerRadius = 7 * cell * 0.35;
		}

		// Outer 7x7
		p.setPen(Qt::NoPen);
		p.setBrush(options.foregroundColor);
		p.drawRoundedRect(finderRect, outerRadius, outerRadius);

		// Inner 5x5 cutout (surface background)
		const auto innerRect = finderRect.adjusted(cell, cell, -cell, -cell);
		const double innerRadius = (outerRadius > 0.0) ? std::max(outerRadius - cell, cell * 0.15) : 0.0;
		p.setBrush(options.cardColor);
		p.drawRoundedRect(innerRect, innerRadius, innerRadius);

		// Center 3x3 eye
		const auto centerRect = finderRect.adjusted(2 * cell, 2 * cell, -2 * cell, -2 * cell);
		double centerRadius = 0.0;
		if (options.dotStyle == DotStyle::Dots) {
			centerRadius = 3 * cell * 0.5;
		} else if (options.dotStyle == DotStyle::Squircles) {
			centerRadius = 3 * cell * 0.35;
		} else if (options.dotStyle == DotStyle::RoundedSquares) {
			centerRadius = 3 * cell * 0.22;
		} else if (options.dotStyle == DotStyle::Md3Shapes) {
			centerRadius = 3 * cell * 0.35;
		}
		p.setBrush(options.foregroundColor);
		p.drawRoundedRect(centerRect, centerRadius, centerRadius);
	};

	// 1. Draw Finder Patterns
	drawFinder(qrLeft, qrTop);
	drawFinder(qrLeft + (n - 7) * cell, qrTop);
	drawFinder(qrLeft, qrTop + (n - 7) * cell);

	// 2. Draw Data Modules
	p.setBrush(options.foregroundColor);
	p.setPen(Qt::NoPen);

	for (int y = 0; y < n; ++y) {
		for (int x = 0; x < n; ++x) {
			if (!matrix.isDark(x, y)) {
				continue;
			}
			if (matrix.isFinder(x, y)) {
				continue;
			}
			const auto modCenter = QPointF(qrLeft + (x + 0.5) * cell, qrTop + (y + 0.5) * cell);
			const double dx = modCenter.x() - center.x();
			const double dy = modCenter.y() - center.y();
			if (options.drawLogo && (dx * dx + dy * dy < cutoutRadiusSq)) {
				continue;
			}

			const auto modRect = QRectF(qrLeft + x * cell, qrTop + y * cell, cell, cell);
			switch (options.dotStyle) {
			case DotStyle::Dots: {
				const double r = cell * 0.44;
				p.drawEllipse(modCenter, r, r);
				break;
			}
			case DotStyle::RoundedSquares: {
				const double pad = cell * 0.05;
				const double r = cell * 0.25;
				p.drawRoundedRect(modRect.adjusted(pad, pad, -pad, -pad), r, r);
				break;
			}
			case DotStyle::Squircles: {
				const double pad = cell * 0.06;
				const double r = cell * 0.38;
				p.drawRoundedRect(modRect.adjusted(pad, pad, -pad, -pad), r, r);
				break;
			}
			case DotStyle::Squares: {
				p.drawRect(modRect);
				break;
			}
			case DotStyle::Diamond: {
				auto diamond = QPainterPath();
				diamond.moveTo(modCenter.x(), modRect.top() + cell * 0.06);
				diamond.lineTo(modRect.right() - cell * 0.06, modCenter.y());
				diamond.lineTo(modCenter.x(), modRect.bottom() - cell * 0.06);
				diamond.lineTo(modRect.left() + cell * 0.06, modCenter.y());
				diamond.closeSubpath();
				p.drawPath(diamond);
				break;
			}
			case DotStyle::Md3Shapes: {
				const int shapeIdx = std::abs((x * 17 + y * 31 + (x ^ y) * 7)) % 4;
				if (shapeIdx == 0) {
					p.drawEllipse(modCenter, cell * 0.44, cell * 0.44);
				} else if (shapeIdx == 1) {
					const double pad = cell * 0.06;
					p.drawRoundedRect(modRect.adjusted(pad, pad, -pad, -pad), cell * 0.38, cell * 0.38);
				} else if (shapeIdx == 2) {
					auto diamond = QPainterPath();
					diamond.moveTo(modCenter.x(), modRect.top() + cell * 0.08);
					diamond.lineTo(modRect.right() - cell * 0.08, modCenter.y());
					diamond.lineTo(modCenter.x(), modRect.bottom() - cell * 0.08);
					diamond.lineTo(modRect.left() + cell * 0.08, modCenter.y());
					diamond.closeSubpath();
					p.drawPath(diamond);
				} else {
					const double pad = cell * 0.05;
					p.drawRoundedRect(modRect.adjusted(pad, pad, -pad, -pad), cell * 0.22, cell * 0.22);
				}
				break;
			}
			}
		}
	}

	// 3. Circular White Cutout Mask & MoltenGram Logo
	if (options.drawLogo) {
		// Clean circular mask
		p.setPen(Qt::NoPen);
		p.setBrush(options.cardColor);
		p.drawEllipse(center, cutoutRadius, cutoutRadius);

		// Outer accent ring
		p.setPen(QPen(QColor(options.accentColor.red(), options.accentColor.green(), options.accentColor.blue(), 75), 1.5));
		p.setBrush(Qt::NoBrush);
		p.drawEllipse(center, cutoutRadius - 1.0, cutoutRadius - 1.0);

		// MoltenGram Logo inside mask
		const double innerLogoR = cutoutRadius * 0.80;
		const auto logoRect = QRectF(center.x() - innerLogoR, center.y() - innerLogoR, 2 * innerLogoR, 2 * innerLogoR);
		PaintLogo(p, logoRect, options.accentColor);
	}
}

void PaintCard(
		QPainter &p,
		const QRect &targetRect,
		const Matrix &matrix,
		const RenderOptions &options) {
	PainterHighQualityEnabler hq(p);

	if (!options.drawCard) {
		PaintQr(p, targetRect, matrix, options);
		return;
	}

	// Card bounds
	const auto cardRect = targetRect.adjusted(12, 10, -12, -44);

	// Soft MD3 Elevation Shadows
	if (options.drawShadow) {
		p.setPen(Qt::NoPen);
		p.setBrush(QColor(0, 0, 0, 18));
		p.drawRoundedRect(cardRect.translated(0, 4).adjusted(-2, -2, 2, 2), options.cardRadius + 2, options.cardRadius + 2);
		p.setBrush(QColor(0, 0, 0, 8));
		p.drawRoundedRect(cardRect.translated(0, 8).adjusted(-4, -4, 4, 4), options.cardRadius + 4, options.cardRadius + 4);
	}

	// Card Background & 1px Surface Outline
	p.setPen(QPen(QColor(0xE0, 0xE0, 0xE6), 1.0));
	p.setBrush(options.cardColor);
	p.drawRoundedRect(cardRect, options.cardRadius, options.cardRadius);

	// Header Title
	if (!options.title.isEmpty()) {
		auto titleFont = p.font();
		titleFont.setPixelSize(15);
		titleFont.setBold(true);
		p.setFont(titleFont);
		p.setPen(options.foregroundColor);
		const auto titleRect = QRect(cardRect.left() + 16, cardRect.top() + 14, cardRect.width() - 32, 24);
		p.drawText(titleRect, Qt::AlignCenter, options.title);
	}

	// QR Placement
	const int qrSide = std::min(options.qrSize, cardRect.width() - 48);
	const int top = options.title.isEmpty()
		? cardRect.top() + (cardRect.height() - qrSide) / 2
		: cardRect.top() + 46;
	const auto qrRect = QRect(cardRect.left() + (cardRect.width() - qrSide) / 2, top, qrSide, qrSide);

	PaintQr(p, qrRect, matrix, options);

	// Subtitle Helper Text
	if (!options.subtitle.isEmpty()) {
		auto subFont = p.font();
		subFont.setPixelSize(11);
		subFont.setBold(false);
		p.setFont(subFont);
		p.setPen(QColor(0x6E, 0x6E, 0x76));
		const auto subRect = QRect(cardRect.left() + 16, qrRect.bottom() + 10, cardRect.width() - 32, 20);
		p.drawText(subRect, Qt::AlignCenter, options.subtitle);
	}
}

QImage RenderCard(
		const QString &link,
		const RenderOptions &options) {
	const auto matrix = GenerateMatrix(link);
	const auto size = QSize(options.cardWidth, options.cardHeight);
	const int ratio = 2; // High-resolution export ratio

	auto image = QImage(size * ratio, QImage::Format_ARGB32_Premultiplied);
	image.fill(Qt::transparent);
	image.setDevicePixelRatio(ratio);

	{
		auto p = QPainter(&image);
		PaintCard(p, QRect(QPoint(), size), matrix, options);
	}

	return image;
}

DotStyle CurrentDotStyle() {
	const int val = Extera::Config::Instance().intValue(kConfigKeyDotStyle);
	if (val >= 0 && val <= 5) {
		return static_cast<DotStyle>(val);
	}
	return DotStyle::Squircles;
}

void SetCurrentDotStyle(DotStyle style) {
	Extera::Config::Instance().setInt(kConfigKeyDotStyle, static_cast<int>(style));
	Extera::Config::Instance().saveNow();
}

QString DotStyleName(DotStyle style) {
	switch (style) {
	case DotStyle::Squircles: return u"Squircles"_q;
	case DotStyle::Dots: return u"Dots"_q;
	case DotStyle::RoundedSquares: return u"Rounded"_q;
	case DotStyle::Squares: return u"Square"_q;
	case DotStyle::Diamond: return u"Diamond"_q;
	case DotStyle::Md3Shapes: return u"MD3"_q;
	}
	return u"Squircles"_q;
}

const std::vector<DotStyle> &AvailableDotStyles() {
	static const auto kList = std::vector<DotStyle>{
		DotStyle::Squircles,
		DotStyle::Dots,
		DotStyle::RoundedSquares,
		DotStyle::Squares,
		DotStyle::Diamond,
		DotStyle::Md3Shapes,
	};
	return kList;
}

void ShowQrShareBox(
		not_null<Window::SessionController*> controller,
		const QString &link,
		const QString &title) {
	const auto effectiveLink = link.trimmed().isEmpty()
		? (controller->session().user()->username().isEmpty()
			? u"https://t.me"_q
			: (u"https://t.me/"_q + controller->session().user()->username()))
		: link.trimmed();

	controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(title.isEmpty()
			? rpl::single(u"Share QR Code"_q)
			: rpl::single(title));

		auto options = RenderOptions();
		options.title = u"MoltenGram"_q;
		options.subtitle = u"Scan to connect on MoltenGram"_q;
		options.dotStyle = CurrentDotStyle();

		const auto cardWidget = box->addRow(
			object_ptr<QrCardWidget>(box, effectiveLink, options));

		box->addButton(rpl::single(u"Copy Link"_q), [=] {
			QGuiApplication::clipboard()->setText(effectiveLink);
			controller->showToast(u"Link copied to clipboard"_q);
		});

		box->addButton(rpl::single(u"Copy Image"_q), [=] {
			const auto img = RenderCard(effectiveLink, cardWidget->currentOptions());
			auto mime = std::make_unique<QMimeData>();
			mime->setImageData(img);
			QGuiApplication::clipboard()->setMimeData(mime.release());
			controller->showToast(u"QR Code copied to clipboard"_q);
		});

		box->addLeftButton(rpl::single(u"Save PNG"_q), [=] {
			const auto path = QFileDialog::getSaveFileName(
				box,
				u"Save MoltenGram QR Code"_q,
				u"moltengram_qr.png"_q,
				u"Images (*.png)"_q);
			if (!path.isEmpty()) {
				const auto img = RenderCard(effectiveLink, cardWidget->currentOptions());
				if (img.save(path)) {
					controller->showToast(u"QR Code saved successfully"_q);
				}
			}
		});

		box->addButton(tr::lng_close(), [=] {
			box->closeBox();
		});
	}));
}

void ShowQrShareBox(
		Window::SessionController *controller,
		const QString &link,
		const QString &title) {
	if (controller) {
		ShowQrShareBox(not_null{ controller }, link, title);
	}
}

} // namespace Extera::QrGenerator
