#include "vectorguideddrawingpane.h"
#include "menubarcommandids.h"
#include "tapp.h"
#include "sceneviewer.h"

#include "toonz/preferences.h"
#include "toonzqt/menubarcommand.h"

#include <QComboBox>
#include <QCheckBox>
#include <QPushButton>
#include <QLabel>
#include <QHBoxLayout>

using namespace DVGui;

//=============================================================================

#if QT_VERSION >= 0x050500
VectorGuidedDrawingPane::VectorGuidedDrawingPane(QWidget *parent,
                                                 Qt::WindowFlags flags)
#else
VectorGuidedDrawingPane::VectorGuidedDrawingPane(QWidget *parent,
                                                 Qt::WindowFlags flags)
#endif
    : QFrame(parent) {

  setObjectName("cornerWidget");
  setObjectName("VectorGuidedDrawingToolbar");

  m_guidedTypeCB = new QComboBox();
  QStringList inputs;
  inputs << tr("Closest") << tr("Farthest") << tr("All");
  m_guidedTypeCB->addItems(inputs);
  int guidedIndex = Preferences::instance()->getGuidedDrawingType() - 1;
  if (guidedIndex < 0) {
    // Was set off, force to Closest
    Preferences::instance()->setValue(guidedDrawingType, 1);
    guidedIndex = 0;
  }
  m_guidedTypeCB->setCurrentIndex(guidedIndex);

  m_autoInbetween = new QCheckBox(tr("Auto Inbetween"), this);
  m_autoInbetween->setChecked(
      Preferences::instance()->getGuidedAutoInbetween());
  connect(m_autoInbetween, SIGNAL(stateChanged(int)), this,
          SLOT(onAutoInbetweenChanged()));

  m_interpolationTypeCB = new QComboBox();
  inputs.clear();
  inputs << tr("Linear") << tr("Ease In") << tr("Ease Out") << tr("EaseIn/Out");
  m_interpolationTypeCB->addItems(inputs);
  int interpolationIndex =
      Preferences::instance()->getGuidedInterpolation() - 1;
  if (interpolationIndex < 0) {
    // Was set off, force to Linear
    Preferences::instance()->setValue(guidedInterpolationType, 1);
    interpolationIndex = 0;
  }
  m_interpolationTypeCB->setCurrentIndex(interpolationIndex);

  QAction *action;
  m_selectPrevGuideBtn = new QPushButton(tr("Previous"), this);
  action = CommandManager::instance()->getAction(MI_SelectPrevGuideStroke);
  m_selectPrevGuideBtn->addAction(action);
  connect(m_selectPrevGuideBtn, SIGNAL(clicked()), action, SLOT(trigger()));

  m_selectNextGuideBtn = new QPushButton(tr("Next"), this);
  action = CommandManager::instance()->getAction(MI_SelectNextGuideStroke);
  m_selectNextGuideBtn->addAction(action);
  connect(m_selectNextGuideBtn, SIGNAL(clicked()), action, SLOT(trigger()));

  m_selectBothGuideBtn = new QPushButton(tr("Both"), this);
  action = CommandManager::instance()->getAction(MI_SelectBothGuideStrokes);
  m_selectBothGuideBtn->addAction(action);
  connect(m_selectBothGuideBtn, SIGNAL(clicked()), action, SLOT(trigger()));

  m_resetGuidesBtn = new QPushButton(tr("Reset"), this);
  action = CommandManager::instance()->getAction(MI_SelectGuideStrokeReset);
  m_resetGuidesBtn->addAction(action);
  connect(m_resetGuidesBtn, SIGNAL(clicked()), action, SLOT(trigger()));

  m_tweenSelectedGuidesBtn =
      new QPushButton(tr("Tween Selected Guide Strokes"), this);
  action = CommandManager::instance()->getAction(MI_TweenGuideStrokes);
  m_tweenSelectedGuidesBtn->addAction(action);
  connect(m_tweenSelectedGuidesBtn, SIGNAL(clicked()), action, SLOT(trigger()));

  m_tweenToSelectedStrokeBtn =
      new QPushButton(tr("Tween Guide Strokes to Selected"), this);
  action = CommandManager::instance()->getAction(MI_TweenGuideStrokeToSelected);
  m_tweenToSelectedStrokeBtn->addAction(action);
  connect(m_tweenToSelectedStrokeBtn, SIGNAL(clicked()), action,
          SLOT(trigger()));

  m_SelectAndTweenBtn =
      new QPushButton(tr("Select Guide Strokes && Tween Mode"), this);
  action = CommandManager::instance()->getAction(MI_SelectGuidesAndTweenMode);
  m_SelectAndTweenBtn->addAction(action);
  connect(m_SelectAndTweenBtn, SIGNAL(clicked()), action, SLOT(trigger()));

  QGridLayout *mainlayout = new QGridLayout();
  mainlayout->setMargin(5);
  mainlayout->setSpacing(2);
  {
    QLabel *guideFrameLabel = new QLabel(this);
    guideFrameLabel->setText(tr("Guide Frames:"));
    mainlayout->addWidget(guideFrameLabel, 0, 0, Qt::AlignRight);
    mainlayout->addWidget(m_guidedTypeCB, 0, 1);

    QLabel *selectGuideStrokeLabel = new QLabel(this);
    selectGuideStrokeLabel->setText(tr("Select Guide Stroke:"));
    mainlayout->addWidget(selectGuideStrokeLabel, 1, 0, Qt::AlignRight);
    QHBoxLayout *buttonlayout = new QHBoxLayout();
    buttonlayout->setMargin(0);
    buttonlayout->setSpacing(2);
    {
      buttonlayout->addWidget(m_selectPrevGuideBtn, 0);
      buttonlayout->addWidget(m_selectNextGuideBtn, 0);
      buttonlayout->addWidget(m_selectBothGuideBtn, 0);
      buttonlayout->addWidget(m_resetGuidesBtn, 0);
    }
    mainlayout->addLayout(buttonlayout, 1, 1);

    mainlayout->addWidget(new DVGui::Separator("", this, true), 2, 0, 1, 2);

    mainlayout->addWidget(m_autoInbetween, 3, 1);

    QLabel *interpolationLabel = new QLabel(this);
    interpolationLabel->setText(tr("Interpolation:"));
    mainlayout->addWidget(interpolationLabel, 4, 0, Qt::AlignRight);
    mainlayout->addWidget(m_interpolationTypeCB, 4, 1);

    mainlayout->addWidget(new DVGui::Separator("", this, true), 5, 0, 1, 2);

    mainlayout->addWidget(m_tweenSelectedGuidesBtn, 6, 0, 1, 2);
    mainlayout->addWidget(m_tweenToSelectedStrokeBtn, 7, 0, 1, 2);
    mainlayout->addWidget(m_SelectAndTweenBtn, 8, 0, 1, 2);
  }

  setLayout(mainlayout);

  connect(m_guidedTypeCB, SIGNAL(currentIndexChanged(int)), this,
          SLOT(onGuidedTypeChanged()));
  connect(m_interpolationTypeCB, SIGNAL(currentIndexChanged(int)), this,
          SLOT(onInterpolationTypeChanged()));

  updateStatus();
}

void VectorGuidedDrawingPane::updateStatus() {
  if (m_guidedTypeCB->currentIndex() == 2) {  // All
    m_selectPrevGuideBtn->setDisabled(true);
    m_selectNextGuideBtn->setDisabled(true);
    m_selectBothGuideBtn->setDisabled(true);
    m_tweenSelectedGuidesBtn->setDisabled(true);
    m_tweenToSelectedStrokeBtn->setDisabled(true);
    m_SelectAndTweenBtn->setDisabled(true);
  } else {  // Closest/Farthest
    m_selectPrevGuideBtn->setDisabled(false);
    m_selectNextGuideBtn->setDisabled(false);
    m_selectBothGuideBtn->setDisabled(false);
    m_tweenSelectedGuidesBtn->setDisabled(false);
    m_tweenToSelectedStrokeBtn->setDisabled(false);
    m_SelectAndTweenBtn->setDisabled(false);
  }
}

void VectorGuidedDrawingPane::onGuidedTypeChanged() {
  int guidedIndex = m_guidedTypeCB->currentIndex() + 1;
  // 1 = closest, 2 = farthest, 3 = all
  Preferences::instance()->setValue(guidedDrawingType, guidedIndex);
  TApp::instance()->getActiveViewer()->update();
  updateStatus();
}

void VectorGuidedDrawingPane::onAutoInbetweenChanged() {
  Preferences::instance()->setValue(guidedAutoInbetween,
                                    m_autoInbetween->isChecked());
}

void VectorGuidedDrawingPane::onInterpolationTypeChanged() {
  int interpolationIndex = m_interpolationTypeCB->currentIndex() + 1;
  // 1 = Linear, 2 = Ease In, 3 = Ease Out, 4 = Ease In/Out
  Preferences::instance()->setValue(guidedInterpolationType,
                                    interpolationIndex);
}
