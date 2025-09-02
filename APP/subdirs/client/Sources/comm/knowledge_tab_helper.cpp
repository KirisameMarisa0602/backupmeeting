#include "knowledge_tab_helper.h"
#include "knowledge_panel.h"

#include <QTabWidget>
#include <QVBoxLayout>
#include <QWidget>
#include <QDebug>

static int findTabIndexByText(QTabWidget* tabs, const QString& text) {
    for (int i = 0; i < tabs->count(); ++i) {
        if (tabs->tabText(i) == text) return i;
    }
    return -1;
}

void AttachKnowledgePanelToTab(QWidget* owner, const QString& tabTitle)
{
    if (!owner) return;

    // 在 owner 的层级里找一个 QTabWidget（若你的对象名是 tabWidget，可用 findChild<QTabWidget*>("tabWidget") 更精确）
    QTabWidget* tabs = owner->findChild<QTabWidget*>();
    if (!tabs) {
        qWarning() << "[KB] AttachKnowledgePanelToTab: no QTabWidget found under" << owner;
        return;
    }

    QWidget* page = nullptr;
    int idx = findTabIndexByText(tabs, tabTitle);
    if (idx >= 0) {
        page = tabs->widget(idx);
    } else {
        page = new QWidget(tabs);
        tabs->addTab(page, tabTitle);
        idx = tabs->indexOf(page);
    }

    if (!page->layout()) {
        auto* lay = new QVBoxLayout(page);
        lay->setContentsMargins(0,0,0,0);
        lay->setSpacing(0);
    }

    // 清掉页面上可能存在的占位子控件（保守做法：仅删除布局里的直接子控件）
    auto* lay = qobject_cast<QVBoxLayout*>(page->layout());
    while (lay && lay->count() > 0) {
        QLayoutItem* item = lay->takeAt(0);
        if (item->widget()) item->widget()->deleteLater();
        delete item;
    }

    auto* kb = new KnowledgePanel(page);
    lay->addWidget(kb);

    qInfo() << "[KB] KnowledgePanel attached into tab" << tabTitle << "at index" << idx;
}
