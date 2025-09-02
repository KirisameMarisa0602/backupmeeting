#pragma once
#include <QString>

class QWidget;

void AttachKnowledgePanelToTab(QWidget* owner, const QString& tabTitle = QStringLiteral("企业知识库"));
