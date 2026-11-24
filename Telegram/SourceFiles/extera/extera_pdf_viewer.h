#pragma once

#include "base/basic_types.h"

#include <QtCore/QString>

namespace Window {
class SessionController;
} // namespace Window

namespace Extera::PdfViewer {

[[nodiscard]] bool Supported();
void Show(
	not_null<Window::SessionController*> controller,
	const QString &path,
	const QString &title);

} // namespace Extera::PdfViewer
