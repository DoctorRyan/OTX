

#include "tools/tool.h"

// TnzTools includes
#include "tools/toolcommandids.h"
#include "tools/toolhandle.h"
#include "tools/cursors.h"
#include "tools/tooloptions.h"
#include "tools/inputmanager.h"

// TnzQt includes
#include "toonzqt/icongenerator.h"

// TnzLib includes
#include "toonzqt/menubarcommand.h"
#include "toonz/txshsimplelevel.h"
#include "toonz/txshleveltypes.h"
#include "toonz/levelproperties.h"
#include "toonz/toonzscene.h"
#include "toonz/sceneproperties.h"
#include "toonz/preferences.h"
#include "toonz/tscenehandle.h"
#include "toonz/txsheethandle.h"
#include "toonz/tframehandle.h"
#include "toonz/tcolumnhandle.h"
#include "toonz/tobjecthandle.h"
#include "toonz/tpalettehandle.h"
#include "toonz/txshlevelhandle.h"
#include "toonz/txshcell.h"
#include "toonz/tstageobject.h"
#include "toonz/tstageobjectspline.h"
#include "toonz/tstageobjecttree.h"
#include "toonz/dpiscale.h"
#include "toonz/palettecontroller.h"

// TnzCore includes
#include "tvectorimage.h"
#include "timagecache.h"
#include "tstroke.h"
#include "tcolorstyles.h"
#include "ttoonzimage.h"
#include "trasterimage.h"

//*****************************************************************************************
//    Local namespace
//*****************************************************************************************

namespace {

// Global variables

typedef std::pair<std::string, TTool::ToolTargetType> ToolKey;
typedef std::multimap<ToolKey, TTool *> ToolTable;
ToolTable *toolTable = 0;

std::set<std::string> *toolNames = 0;

//===================================================================

// Local classes

struct DummyTool final : public TTool {
  ToolType getToolType() const override {
    return TTool::LevelReadTool;
  }  // Test level type
  ToolTargetType getTargetType() const {
    return TTool::NoTarget;
  }  // Works on nothing
  int getCursorId() const override {
    return ToolCursor::ForbiddenCursor;
  }  // Forbids everything

  DummyTool() : TTool("T_Dummy") {}

} theDummyTool;

//-------------------------------------------------------------------

class ToolSelector {
  std::string m_toolName;

public:
  ToolSelector(std::string toolName) : m_toolName(toolName) {}

  void selectTool() {
    TTool::Application *app = TTool::getApplication();
    if (app) app->getCurrentTool()->setTool(QString::fromStdString(m_toolName));
  }
};

//===================================================================

// Local functions

TFrameId getNewFrameId(TXshSimpleLevel *sl, int row) {
  TFrameId fid(row + 1);
  if (sl->isFid(fid)) {
    fid = TFrameId(fid.getNumber(), 'a');
    while (fid.getLetter() < 'z' && sl->isFid(fid))
      fid = TFrameId(fid.getNumber(), fid.getLetter() + 1);
  }
  return fid;
}

}  // namespace

//*****************************************************************************************
//    TTool  static members
//*****************************************************************************************

TTool::Application *TTool::m_application   = 0;
std::set<TFrameId> TTool::m_selectedFrames = std::set<TFrameId>();
bool TTool::m_isLevelCreated               = false;
bool TTool::m_isFrameCreated               = false;

// m_cellsData
// brutto brutto. fix quick & dirty del baco #6213 (undo con animation sheet)
// bisogna ripensare la logica degli undo e del touchImage
// m_cellsData viene inizializzato nel touchImage() in modalita' animation sheet
// contiene una o due terne che rappresentano range di celle (dell'xsheet)
// modificate dall'inserimento
// di un nuovo frame: [r0,r1,type].
// type = 0 : vecchio (cella[r0-1]) => nuovo
// type = 1 : vuoto => vecchio (cella[r0-1])
// type = 2 : vuoto => nuovo
// cfr. il codice di TTool::touchImage()
// ToolUtils::TToolUndo::removeLevelAndFrameIfNeeded()

std::vector<int> TTool::m_cellsData;

//*****************************************************************************************
//    TTool  implementation
//*****************************************************************************************

TTool::TTool(std::string name)
    : m_name(name)
    , m_viewer(0)
    , m_targetType(NoTarget)
    , m_enabled(true)
    , m_active(false)
    , m_picking(false) {}

//-------------------------------------------------------------------

TTool *TTool::getTool(std::string toolName, ToolTargetType targetType) {
  if (!toolTable) return 0;

  // if to this name and target type was assigned more then one tool
  // then select tool which more compatible with default target type

  int defTarget = 0;
  switch(Preferences::instance()->getDefLevelType()) {
  case PLI_XSHLEVEL:  defTarget = VectorImage; break;
  case TZP_XSHLEVEL:  defTarget = ToonzImage;  break;
  case OVL_XSHLEVEL:  defTarget = RasterImage; break;
  case META_XSHLEVEL: defTarget = MetaImage;   break;
  default:            defTarget = 0;           break;
  }

  bool isDefault = false;
  int target = 0;
  TTool *tool = 0;

  std::pair<ToolTable::iterator, ToolTable::iterator> range =
    toolTable->equal_range(std::make_pair(toolName, targetType));
  for(ToolTable::iterator it = range.first; it != range.second; ++it) {
    int t = it->second->getTargetType();
    bool d = (bool)(t & defTarget);
    if (!tool || (d && !isDefault) || (d == isDefault && t > target)) {
      isDefault = d;
      target = t;
      tool = it->second;
    }
  }

  return tool;
}

//-----------------------------------------------------------------------------

void TTool::bind(int targetType) {
  m_targetType = targetType;

  if (!toolTable) toolTable = new ToolTable();

  if (!toolNames) toolNames = new std::set<std::string>();

  ToolTargetType targets[] = {
    EmptyTarget,
    ToonzImage,
    VectorImage,
    RasterImage,
    MeshImage,
    MetaImage };
  int targetsCount = sizeof(targets)/sizeof(*targets);

  std::string name = getName();
  if (toolNames->count(name) == 0) {
    toolNames->insert(name);

    // Initialize with the dummy tool
    for(int i = 0; i < targetsCount; ++i)
      if (!toolTable->count(std::make_pair(name, targets[i])))
        toolTable->insert(
          std::make_pair(std::make_pair(name, targets[i]), &theDummyTool));

    ToolSelector *toolSelector = new ToolSelector(name);
    CommandManager::instance()->setHandler(
        name.c_str(), new CommandHandlerHelper<ToolSelector>(
                          toolSelector, &ToolSelector::selectTool));
  }

  for(int i = 0; i < targetsCount; ++i)
    if (targetType & targets[i])
      toolTable->insert(
        std::make_pair(std::make_pair(name, targets[i]), this));
}

//-----------------------------------------------------------------------------

ToolOptionsBox *TTool::createOptionsBox() {
  TPaletteHandle *currPalette =
      m_application->getPaletteController()->getCurrentLevelPalette();
  ToolHandle *currTool = m_application->getCurrentTool();
  return new GenericToolOptionsBox(0, this, currPalette, 0, currTool);
}

//-----------------------------------------------------------------------------

double TTool::getPixelSize() const {
  return m_viewer ? m_viewer->getPixelSize() : 1.0;
}

//-----------------------------------------------------------------------------

TXshCell TTool::getImageCell() {
  assert(m_application);

  TXshCell result;

  TFrameHandle *currentFrame    = m_application->getCurrentFrame();
  TXshLevelHandle *currentLevel = m_application->getCurrentLevel();

  if (currentFrame->isEditingLevel()) {
    if (TXshLevel *xl = currentLevel->getLevel()) {
      if (TXshSimpleLevel *sl = xl->getSimpleLevel()) {
        result.m_level   = xl;
        result.m_frameId = currentFrame->getFid();
      }
    }
  } else {
    if (TXsheet *xsh = m_application->getCurrentXsheet()->getXsheet()) {
      if (!m_application->getCurrentObject()->isSpline()) {
        int row = currentFrame->getFrame();
        int col = m_application->getCurrentColumn()->getColumnIndex();

        result = xsh->getCell(row, col);
      }
    }
  }

  return result;
}

//-----------------------------------------------------------------------------

TImage *TTool::getImage(bool toBeModified, int subsampling) {
  assert(m_application);

  if (m_application->getCurrentFrame()->isPlaying())
    toBeModified =
        false;  // In playback mode, you are not going to modify images
                // Probably useless - tools are disabled when playing...
  const TXshCell &cell = getImageCell();
  if (cell.isEmpty()) {
    TObjectHandle *currentObject = m_application->getCurrentObject();
    return currentObject->isSpline() ? currentObject->getSplineImage()
                                     : (TImage *)0;
  } else
    return cell.getImage(toBeModified, subsampling).getPointer();
}

//-----------------------------------------------------------------------------

TImage *TTool::touchImage() {
  if (!m_application) return 0;

  m_cellsData.clear();

  m_isLevelCreated  = false;
  m_isFrameCreated  = false;
  Preferences *pref = Preferences::instance();

  bool isAutoCreateEnabled   = pref->isAutoCreateEnabled();
  bool animationSheetEnabled = pref->isAnimationSheetEnabled();
  bool isAutoStretchEnabled  = pref->isAutoStretchEnabled();

  TFrameHandle *currentFrame    = m_application->getCurrentFrame();
  TXshLevelHandle *currentLevel = m_application->getCurrentLevel();

  if (currentFrame->isEditingLevel()) {
    // Editing level

    // no level => return 0
    TXshLevel *xl = currentLevel->getLevel();
    if (!xl) return 0;
    TXshSimpleLevel *sl = xl->getSimpleLevel();
    if (!sl || sl->isEmpty()) return 0;

    TFrameId fid = currentFrame->getFid();
    TImageP img  = sl->getFrame(fid, true);
    if (!img) {
      // no drawing found
      if (sl->isSubsequence() || sl->isReadOnly() || !isAutoCreateEnabled)
        return 0;

      // create a new drawing
      img = sl->createEmptyFrame();
      sl->setFrame(fid, img);
      currentLevel->notifyLevelChange();
      m_isFrameCreated = true;
    }
    return img.getPointer();
  } else {
    // editing xsheet
    if (m_application->getCurrentObject()->isSpline()) return 0;

    TSceneHandle *currentScene = m_application->getCurrentScene();
    ToonzScene *scene          = currentScene->getScene();
    int row                    = currentFrame->getFrame();
    int col = m_application->getCurrentColumn()->getColumnIndex();
    if (col < 0) return 0;

    TXsheetHandle *currentXsheet = m_application->getCurrentXsheet();
    TXsheet *xsh                 = currentXsheet->getXsheet();
    if (!xsh) return 0;

    TXshCell cell       = xsh->getCell(row, col);
    TXshSimpleLevel *sl = cell.getSimpleLevel();

    if (sl != 0) {
      // current cell is not empty
      if (isAutoCreateEnabled && animationSheetEnabled && row > 0 &&
          xsh->getCell(row - 1, col) == xsh->getCell(row, col)) {
        // animationSheet is enabled and the current cell is a "hold". We must
        // create a new drawing.
        // measure the hold length (starting from the current row) : r0-r1
        int r0 = row, r1 = row;
        if (isAutoStretchEnabled)
          while (xsh->getCell(r1 + 1, col) == cell) r1++;
        // find the proper frameid (possibly addisng suffix, in order to avoid a
        // fid already used)
        TFrameId fid = getNewFrameId(sl, row);
        // create the new drawing
        TImageP img      = sl->createEmptyFrame();
        m_isFrameCreated = true;
        // insert the drawing in the level
        sl->setFrame(fid, img);
        // update the cell
        cell = TXshCell(sl, fid);
        // update the xsheet (change the current cell and possibly all the
        // following "hold")
        for (int r = r0; r <= r1; r++) xsh->setCell(r, col, cell);
        // notify
        currentXsheet->notifyXsheetChanged();
        currentScene->notifyCastChange();
        currentLevel->notifyLevelChange();
        m_cellsData.push_back(r0);
        m_cellsData.push_back(r1);
        m_cellsData.push_back(0);
      }
      // we've found the image. return it.
      return cell.getImage(true).getPointer();
    }

    // current cell is empty.
    if (!isAutoCreateEnabled) return 0;

    // get the column range
    int r0, r1;
    xsh->getCellRange(col, r0, r1);

    if (animationSheetEnabled && r0 <= r1) {
      // animation sheet enabled and not empty column. We must create a new
      // drawing in the column level and possibly add "holds"

      // find the last not-empty cell before the current one (a) and the first
      // after (b)
      int a = row - 1, b = row + 1;
      while (a >= r0 && xsh->getCell(a, col).isEmpty()) a--;
      while (b <= r1 && xsh->getCell(b, col).isEmpty()) b++;

      // find the level we must attach to
      if (a >= r0) {
        // there is a not-emtpy cell before the current one
        sl = xsh->getCell(a, col).getSimpleLevel();
      } else if (b <= r1) {
        sl = xsh->getCell(b, col).getSimpleLevel();
      }
      if (sl) {
        // note: sl should be always !=0 (the column is not empty)
        // if - for some reason - it is ==0 then we skip to the standard (i.e.
        // !animationSheetEnabled) beahviour

        // create the drawing
        // find the proper frameid (possibly addisng suffix, in order to avoid a
        // fid already used)
        TFrameId fid = getNewFrameId(sl, row);
        // create the new drawing
        TImageP img      = sl->createEmptyFrame();
        m_isFrameCreated = true;
        // insert the drawing in the level
        sl->setFrame(fid, img);
        // update the cell
        cell = TXshCell(sl, fid);
        xsh->setCell(row, col, cell);

        // create holds
        if (!isAutoStretchEnabled) {
          m_cellsData.push_back(row);
          m_cellsData.push_back(row);
          m_cellsData.push_back(2);  // vuoto => nuovo
        } else {
          if (a >= r0) {
            // create a hold before : [a+1, row-1]
            TXshCell aCell = xsh->getCell(a, col);
            for (int i = a + 1; i < row; i++) xsh->setCell(i, col, aCell);
            m_cellsData.push_back(a + 1);
            m_cellsData.push_back(row - 1);
            m_cellsData.push_back(1);  // vuoto => vecchio

            if (b <= r1 && xsh->getCell(b, col).getSimpleLevel() == sl) {
              // create also a hold after
              for (int i = row + 1; i < b; i++) xsh->setCell(i, col, cell);
              m_cellsData.push_back(row);
              m_cellsData.push_back(b - 1);
              m_cellsData.push_back(2);  // vuoto => nuovo
            } else {
              m_cellsData.push_back(row);
              m_cellsData.push_back(row);
              m_cellsData.push_back(2);  // vuoto => nuovo
            }
          } else if (b <= r1) {
            // create a hold after
            for (int i = row + 1; i < b; i++) xsh->setCell(i, col, cell);
            m_cellsData.push_back(row);
            m_cellsData.push_back(b - 1);
            m_cellsData.push_back(2);  // vuoto => nuovo
          }
        }
      }
      // notify & return
      currentXsheet->notifyXsheetChanged();
      currentScene->notifyCastChange();
      currentLevel->notifyLevelChange();
      return cell.getImage(true).getPointer();
    }

    if (row > 0 && xsh->getCell(row - 1, col).getSimpleLevel() != 0 &&
        !animationSheetEnabled) {
      sl = xsh->getCell(row - 1, col).getSimpleLevel();
      if (sl->getType() != OVL_XSHLEVEL ||
          sl->getPath().getFrame() != TFrameId::NO_FRAME) {
        // la cella precedente contiene un drawing di un livello. animationSheet
        // e' disabilitato
        // creo un nuovo frame
        currentLevel->setLevel(sl);
        if (sl->isSubsequence() || sl->isReadOnly()) return 0;
        TFrameId fid     = sl->index2fid(sl->getFrameCount());
        TImageP img      = sl->createEmptyFrame();
        m_isFrameCreated = true;
        sl->setFrame(fid, img);
        cell = TXshCell(sl, fid);
        xsh->setCell(row, col, cell);
        currentXsheet->notifyXsheetChanged();
        currentScene->notifyCastChange();
        currentLevel->notifyLevelChange();
        return img.getPointer();
      }
    }

    // animation sheet disabled or empty column. autoCreate is enabled: we must
    // create a new level

    // select one from supported level types
    // default level type is preffered

    int levelType = pref->getDefLevelType();
    int toolLevelType = UNKNOWN_XSHLEVEL;
    bool found = false;

    if ( m_targetType & MetaImage )
      { toolLevelType = META_XSHLEVEL; found = found || toolLevelType == levelType; }
    if ( m_targetType & RasterImage )
      { toolLevelType = OVL_XSHLEVEL;  found = found || toolLevelType == levelType; }
    if ( m_targetType & ToonzImage )
      { toolLevelType = TZP_XSHLEVEL;  found = found || toolLevelType == levelType; }
    if ( m_targetType & VectorImage )
      { toolLevelType = PLI_XSHLEVEL;  found = found || toolLevelType == levelType; }

    if (toolLevelType == UNKNOWN_XSHLEVEL) return 0;
    if (!found) levelType = toolLevelType;

    TXshLevel *xl    = scene->createNewLevel(levelType);
    sl               = xl->getSimpleLevel();
    m_isLevelCreated = true;

    // create the drawing
    TFrameId fid = animationSheetEnabled ? getNewFrameId(sl, row) : TFrameId(1);
    TImageP img  = sl->createEmptyFrame();
    m_isFrameCreated = true;
    sl->setFrame(fid, img);
    cell = TXshCell(sl, fid);
    xsh->setCell(row, col, cell);
    if (animationSheetEnabled) {
      m_cellsData.push_back(row);
      m_cellsData.push_back(row);
      m_cellsData.push_back(2);  // vuoto => nuovo
    }

    currentXsheet->notifyXsheetChanged();
    currentScene->notifyCastChange();
    currentLevel->notifyLevelChange();
    return img.getPointer();
  }
}

//-----------------------------------------------------------------------------

void TTool::updateToolsPropertiesTranslation() {
  ToolTable::iterator tt, tEnd(toolTable->end());
  for (tt = toolTable->begin(); tt != tEnd; ++tt)
    tt->second->updateTranslation();
}

//-----------------------------------------------------------------------------

void TTool::invalidate(const TRectD &rect) {
  if (m_viewer) {
    if (rect.isEmpty())
      m_viewer->GLInvalidateAll();
    else {
      TPointD dpiScale(1, 1);
      TXshSimpleLevel *sl =
          getApplication()->getCurrentLevel()->getSimpleLevel();
      if (sl) dpiScale = getCurrentDpiScale(sl, getCurrentFid());
      m_viewer->GLInvalidateRect(getCurrentColumnMatrix() *
                                 TScale(dpiScale.x, dpiScale.y) * rect);
    }
  }
}

//-----------------------------------------------------------------------------

int TTool::pick(const TPointD &p) {
  if (!m_viewer) return 0;

  m_picking = true;
  int ret   = m_viewer->pick(p);
  m_picking = false;

  return ret;
}

//-----------------------------------------------------------------------------

TMouseEvent
TTool::makeMouseEvent() {
  TToolViewer *viewer = getViewer();
  TInputManager *manager = viewer ? viewer->getInputManager() : 0;

  TPointD point = manager && !manager->getOutputHovers().empty()
                ? manager->getOutputHovers().front() : TPointD();
  TPointD pos = manager ? manager->toolToScreen() * point : point;
  TDimensionI size = viewer ? viewer->getWindowSize() : TDimensionI();
  TPointD center(0.5*(double)size.lx, 0.5*(double)size.ly);


  TMouseEvent e;
  e.m_pos = pos + center;
  if (manager) {
    e.setModifiers( manager->state.isKeyPressed(TKey::shift),
                    manager->state.isKeyPressed(TKey::alt),
                    manager->state.isKeyPressed(TKey::control) );
    if (manager->state.isButtonPressedAny(Qt::LeftButton   )) e.m_buttons |= Qt::LeftButton;
    if (manager->state.isButtonPressedAny(Qt::RightButton  )) e.m_buttons |= Qt::RightButton;
    if (manager->state.isButtonPressedAny(Qt::MidButton    )) e.m_buttons |= Qt::MidButton;
    if (manager->state.isButtonPressedAny(Qt::BackButton   )) e.m_buttons |= Qt::BackButton;
    if (manager->state.isButtonPressedAny(Qt::ForwardButton)) e.m_buttons |= Qt::ForwardButton;
    if (manager->state.isButtonPressedAny(Qt::TaskButton   )) e.m_buttons |= Qt::TaskButton;
  }
  e.m_mousePos = QPointF(pos.x + center.x, center.y - pos.y);
  return e;
}

//-----------------------------------------------------------------------------

TMouseEvent
TTool::makeMouseEvent(const TTrackPoint &point, const TTrack &track) {
  TToolViewer *viewer = getViewer();
  TInputManager *manager = viewer ? viewer->getInputManager() : 0;

  TDimensionI size = viewer ? viewer->getWindowSize() : TDimensionI();
  TPointD center(0.5*(double)size.lx, 0.5*(double)size.ly);

  TInputState::KeyState::Holder keyState = track.getKeyState(point.time);
  TInputState::ButtonState::Holder buttonState = track.getButtonState(point.time);

  TMouseEvent e;
  e.m_pos = point.screenPosition + center;
  e.m_pressure = track.hasPressure ? point.pressure : 1.0;
  e.setModifiers( keyState.isPressed(TKey::shift),
                  keyState.isPressed(TKey::alt),
                  keyState.isPressed(TKey::control) );
  if (buttonState.isPressed(Qt::LeftButton   )) e.m_buttons |= Qt::LeftButton;
  if (buttonState.isPressed(Qt::RightButton  )) e.m_buttons |= Qt::RightButton;
  if (buttonState.isPressed(Qt::MidButton    )) e.m_buttons |= Qt::MidButton;
  if (buttonState.isPressed(Qt::BackButton   )) e.m_buttons |= Qt::BackButton;
  if (buttonState.isPressed(Qt::ForwardButton)) e.m_buttons |= Qt::ForwardButton;
  if (buttonState.isPressed(Qt::TaskButton   )) e.m_buttons |= Qt::TaskButton;
  e.m_mousePos = QPointF(point.screenPosition.x + center.x, center.y - point.screenPosition.y);
  e.m_isTablet = track.hasPressure;
  return e;
}

//-----------------------------------------------------------------------------

bool
TTool::keyEvent(
  bool press,
  TInputState::Key key,
  QKeyEvent *event,
  const TInputManager &manager )
{
  if (press && !key.isModifier())
    return keyDown(event);
  return false;
}

//-----------------------------------------------------------------------------

void
TTool::buttonEvent(
  bool press,
  TInputState::DeviceId device,
  TInputState::Button button,
  const TInputManager &manager )
{
  if (press && button == Qt::RightButton && !manager.getOutputHovers().empty()) {
    TMouseEvent e = makeMouseEvent();
    e.m_button = Qt::RightButton;
    rightButtonDown(manager.getOutputHovers().front(), e);
  }
}

//-----------------------------------------------------------------------------

void
TTool::hoverEvent(const TInputManager &manager) {
  if (!manager.getOutputHovers().empty())
    mouseMove(manager.getOutputHovers().front(), makeMouseEvent());
}

//-----------------------------------------------------------------------------

void
TTool::doubleClickEvent(const TInputManager &manager) {
  if (!manager.getOutputHovers().empty())
    leftButtonDoubleClick(manager.getOutputHovers().front(), makeMouseEvent());
}

//-----------------------------------------------------------------------------

void
TTool::paintTrackBegin(const TTrackPoint &point, const TTrack &track, bool firstTrack)
  { if (firstTrack) leftButtonDown(point.position, makeMouseEvent(point, track)); }

//-----------------------------------------------------------------------------

void
TTool::paintTrackMotion(const TTrackPoint &point, const TTrack &track, bool firstTrack)
  { if (firstTrack) leftButtonDrag(point.position, makeMouseEvent(point, track)); }

//-----------------------------------------------------------------------------

void
TTool::paintTrackEnd(const TTrackPoint &point, const TTrack &track, bool firstTrack)
  { if (firstTrack) leftButtonUp(point.position, makeMouseEvent(point, track)); }

//-----------------------------------------------------------------------------

void
TTool::paintTrackPoint(const TTrackPoint &point, const TTrack &track, bool firstTrack) {
  if (track.pointsAdded == track.size())
    paintTrackBegin(point, track, firstTrack);
  else
  if (point.final)
    paintTrackEnd(point, track, firstTrack);
  else
    paintTrackMotion(point, track, firstTrack);
}

//-----------------------------------------------------------------------------

void
TTool::paintTracks(const TTrackList &tracks) {
  // paint track points in chronological order
  while(true) {
    TTrackP track;
    TTimerTicks minTicks = 0;
    double minTimeOffset = 0.0;
    for(TTrackList::const_iterator i = tracks.begin(); i != tracks.end(); ++i) {
      const TTrack &t = **i;
      if (t.pointsAdded > 0) {
        TTimerTicks ticks = t.ticks();
        double timeOffset = t.timeOffset() + t.current().time;
        if (!track || (ticks - minTicks)*TToolTimer::frequency + timeOffset - minTimeOffset < 0.0) {
          track = *i;
          minTicks = ticks;
          minTimeOffset = timeOffset;
        }
      }
    }
    if (!track) break;
    paintTrackPoint(track->current(), *track, track == tracks.front());
    --track->pointsAdded;
  }
}

//-----------------------------------------------------------------------------

int
TTool::paintApply(int count) {
  if (count <= 0) return 0;
  for(int i = 0; i < count; ++i)
    if (!paintApply())
      return i;
  return count;
}

//-----------------------------------------------------------------------------

void
TTool::paintPop(int count) {
  for(int i = 0; i < count; ++i)
    paintPop();
}

//-----------------------------------------------------------------------------

TXsheet *TTool::getXsheet() const {
  if (!m_application) return 0;
  return m_application->getCurrentXsheet()->getXsheet();
}

//-----------------------------------------------------------------------------

int TTool::getFrame() {
  if (!m_application) return 0;
  return m_application->getCurrentFrame()->getFrame();
}

//-----------------------------------------------------------------------------

int TTool::getColumnIndex() {
  if (!m_application) return 0;
  return m_application->getCurrentColumn()->getColumnIndex();
}

//-----------------------------------------------------------------------------

TStageObjectId TTool::getObjectId() {
  if (!m_application) return TStageObjectId();
  return m_application->getCurrentObject()->getObjectId();
}

//------------------------------------------------------------

TTool::Application *TTool::getApplication() {
  if (m_application == 0)
    assert(!"you MUST call the TTool::setApplication function in the main of the program!");
  return m_application;
}

//-----------------------------------------------------------------------------

/*! Notify change of current image: update icon and notify level change.
    If current object is a spline commit spline chenged.
    If current mode is EditingLevel touch current frame.
*/
void TTool::notifyImageChanged() {
  onImageChanged();

  if (!m_application) return;

  m_application->getCurrentScene()->setDirtyFlag(true);
  if (m_application->getCurrentFrame()->isEditingLevel()) {
    TXshLevel *xl = m_application->getCurrentLevel()->getLevel();
    if (!xl) return;
    TXshSimpleLevel *sl = xl->getSimpleLevel();
    if (!sl) return;
    TFrameId fid = m_application->getCurrentFrame()->getFid();
    sl->touchFrame(fid);
    // sl->setDirtyFlag(true);
    IconGenerator::instance()->invalidate(sl, fid);
    IconGenerator::instance()->invalidateSceneIcon();
  } else {
    TXsheet *xsh = m_application->getCurrentXsheet()->getXsheet();
    if (!xsh) return;

    TObjectHandle *currentObject = m_application->getCurrentObject();

    if (currentObject->isSpline()) {
      m_application->getCurrentObject()->commitSplineChanges();
      TStageObject *pegbar = xsh->getStageObject(currentObject->getObjectId());
      IconGenerator::instance()->invalidate(pegbar->getSpline());
    } else {
      int row = m_application->getCurrentFrame()->getFrame();
      int col = m_application->getCurrentColumn()->getColumnIndex();
      if (col < 0) return;
      TXshCell cell       = xsh->getCell(row, col);
      TXshSimpleLevel *sl = cell.getSimpleLevel();
      if (sl) {
        IconGenerator::instance()->invalidate(sl, cell.m_frameId);
        sl->touchFrame(cell.m_frameId);
        IconGenerator::instance()->invalidateSceneIcon();
      }
    }
  }
  m_application->getCurrentLevel()->notifyLevelChange();
}

//-----------------------------------------------------------------------------

/*! Notify change of image in \b fid: update icon and notify level change.
 */
void TTool::notifyImageChanged(const TFrameId &fid) {
  onImageChanged();

  if (!m_application) return;

  m_application->getCurrentScene()->setDirtyFlag(true);
  if (m_application->getCurrentFrame()->isEditingLevel()) {
    TXshLevel *xl = m_application->getCurrentLevel()->getLevel();
    if (!xl) return;
    TXshSimpleLevel *sl = xl->getSimpleLevel();
    if (!sl) return;
    sl->setDirtyFlag(true);
    IconGenerator::instance()->invalidate(sl, fid);
    IconGenerator::instance()->invalidateSceneIcon();
  } else {
    int row = m_application->getCurrentFrame()->getFrame();
    int col = m_application->getCurrentColumn()->getColumnIndex();
    if (col < 0) return;
    TXsheet *xsh = m_application->getCurrentXsheet()->getXsheet();
    if (!xsh) return;
    TXshCell cell       = xsh->getCell(row, col);
    TXshSimpleLevel *sl = cell.getSimpleLevel();
    if (sl) {
      IconGenerator::instance()->invalidate(sl, fid);
      IconGenerator::instance()->invalidateSceneIcon();
      sl->setDirtyFlag(true);
    }
  }
  m_application->getCurrentLevel()->notifyLevelChange();
}

//-----------------------------------------------------------------------------

TFrameId TTool::getCurrentFid() const {
  if (!m_application) return TFrameId();

  TFrameHandle *fh = m_application->getCurrentFrame();

  if (fh->isEditingLevel()) return fh->getFid();

  int row = m_application->getCurrentFrame()->getFrame();
  int col = m_application->getCurrentColumn()->getColumnIndex();
  TXshCell cell =
      m_application->getCurrentXsheet()->getXsheet()->getCell(row, col);
  if (cell.isEmpty()) return TFrameId::NO_FRAME;

  return cell.getFrameId();
}

//-----------------------------------------------------------------------------

TAffine TTool::getCurrentColumnMatrix() const {
  return getColumnMatrix(m_application->getCurrentColumn()->getColumnIndex());
}

//-----------------------------------------------------------------------------

TAffine TTool::getCurrentColumnParentMatrix() const {
  if (!m_application) return TAffine();

  TFrameHandle *fh = m_application->getCurrentFrame();
  if (fh->isEditingLevel()) return TAffine();
  int frame       = fh->getFrame();
  int columnIndex = m_application->getCurrentColumn()->getColumnIndex();
  TXsheet *xsh    = m_application->getCurrentXsheet()->getXsheet();
  TStageObjectId parentId =
      xsh->getStageObjectParent(TStageObjectId::ColumnId(columnIndex));
  return xsh->getPlacement(parentId, frame);
}

//-----------------------------------------------------------------------------

TAffine TTool::getCurrentObjectParentMatrix() const {
  if (!m_application) return TAffine();

  TXsheet *xsh = m_application->getCurrentXsheet()->getXsheet();
  int frame    = m_application->getCurrentFrame()->getFrame();
  TStageObjectId currentObjectId =
      m_application->getCurrentObject()->getObjectId();
  if (currentObjectId == TStageObjectId::NoneId) return TAffine();
  TStageObjectId parentId = xsh->getStageObjectParent(currentObjectId);
  if (parentId == TStageObjectId::NoneId)
    return TAffine();
  else
    return xsh->getPlacement(parentId, frame);
}

//-----------------------------------------------------------------------------

TAffine TTool::getColumnMatrix(int columnIndex) const {
  if (!m_application) return TAffine();

  TFrameHandle *fh = m_application->getCurrentFrame();
  if (fh->isEditingLevel()) return TAffine();
  int frame    = fh->getFrame();
  TXsheet *xsh = m_application->getCurrentXsheet()->getXsheet();
  TStageObjectId columnObjId =
      (columnIndex >= 0)
          ? TStageObjectId::ColumnId(columnIndex)
          : TStageObjectId::CameraId(xsh->getCameraColumnIndex());
  TAffine columnPlacement = xsh->getPlacement(columnObjId, frame);
  double columnZ          = xsh->getZ(columnObjId, frame);

  TStageObjectId cameraId = xsh->getStageObjectTree()->getCurrentCameraId();
  TStageObject *camera    = xsh->getStageObject(cameraId);
  TAffine cameraPlacement = camera->getPlacement(frame);
  double cameraZ          = camera->getZ(frame);

  TStageObject *object = xsh->getStageObject(columnObjId);
  TAffine placement;
  TStageObject::perspective(placement, cameraPlacement, cameraZ,
                            columnPlacement, columnZ,
                            object->getGlobalNoScaleZ());

  return placement;
}

//-----------------------------------------------------------------------------

TAffine TTool::getCurrentObjectParentMatrix2() const {
  TTool::Application *app = m_application;
  TFrameHandle *fh        = app->getCurrentFrame();
  if (fh->isEditingLevel()) return TAffine();
  int frame               = fh->getFrame();
  TXsheet *xsh            = app->getCurrentXsheet()->getXsheet();
  TStageObjectId id       = app->getCurrentObject()->getObjectId();
  double objZ             = xsh->getZ(id, frame);
  TStageObjectId parentId = xsh->getStageObjectParent(id);
  if (parentId == TStageObjectId::NoneId) return TAffine();
  id                   = parentId;
  TAffine objPlacement = xsh->getPlacement(id, frame);

  TStageObjectId cameraId = xsh->getStageObjectTree()->getCurrentCameraId();
  TStageObject *camera    = xsh->getStageObject(cameraId);
  TAffine cameraPlacement = camera->getPlacement(frame);
  double cameraZ          = camera->getZ(frame);

  TAffine placement;
  TStageObject::perspective(placement, cameraPlacement, cameraZ, objPlacement,
                            objZ, 0);
  return placement;
}

//-----------------------------------------------------------------------------

void TTool::updateMatrix() {
  assert(m_application);

  if (m_application->getCurrentObject()->isSpline())
    setMatrix(getCurrentObjectParentMatrix2());
  else
    setMatrix(getCurrentColumnMatrix());
}

//-----------------------------------------------------------------------------

void TTool::resetInputMethod() {
  if (m_viewer) m_viewer->resetInputMethod();
}

//-----------------------------------------------------------------------------

bool TTool::isColumnLocked(int columnIndex) const {
  if (columnIndex < 0) return false;
  TXsheet *xsh       = getXsheet();
  TXshColumn *column = xsh->getColumn(columnIndex);
  if (!column) return false;
  return column->isLocked();
}

//-----------------------------------------------------------------------------

QString TTool::updateEnabled() {
  int rowIndex    = m_application->getCurrentFrame()->getFrame();
  int columnIndex = m_application->getCurrentColumn()->getColumnIndex();

  return updateEnabled(rowIndex, columnIndex);
}

QString TTool::updateEnabled(int rowIndex, int columnIndex) {
  // Disable every tool during playback
  if (m_application->getCurrentFrame()->isPlaying())
    return (enable(false), QString());

  // Release Generic tools at once
  int toolType   = getToolType();
  int targetType = getTargetType();

  if (toolType == TTool::GenericTool) return (enable(true), QString());

  // Retrieve vars and view modes
  TXsheet *xsh = m_application->getCurrentXsheet()->getXsheet();

  TXshColumn *column = (columnIndex >= 0) ? xsh->getColumn(columnIndex) : 0;

  TXshLevel *xl       = m_application->getCurrentLevel()->getLevel();
  TXshSimpleLevel *sl = xl ? xl->getSimpleLevel() : 0;
  int levelType       = sl ? sl->getType() : NO_XSHLEVEL;

  // If not in Level editor, let's use our current cell from the xsheet to
  // find the nearest level before it
  if (levelType == NO_XSHLEVEL &&
      !m_application->getCurrentFrame()->isEditingLevel()) {
    TXshCell cell = xsh->getCell(rowIndex, columnIndex);
    xl            = cell.isEmpty() ? 0 : (TXshLevel *)(&cell.m_level);
    sl            = cell.isEmpty() ? 0 : cell.getSimpleLevel();
    levelType     = cell.isEmpty() ? NO_XSHLEVEL : cell.m_level->getType();
  }

  if (Preferences::instance()->isAutoCreateEnabled() &&
      Preferences::instance()->isAnimationSheetEnabled()) {
    // If not in Level editor, let's use our current cell from the xsheet to
    // find the nearest level before it
    if (levelType == NO_XSHLEVEL &&
        !m_application->getCurrentFrame()->isEditingLevel()) {
      int r0, r1;
      xsh->getCellRange(columnIndex, r0, r1);
      for (int r = std::min(r1, rowIndex); r > r0; r--) {
        TXshCell cell = xsh->getCell(r, columnIndex);
        if (cell.isEmpty()) continue;
        xl        = (TXshLevel *)(&cell.m_level);
        sl        = cell.getSimpleLevel();
        levelType = cell.m_level->getType();
        break;
      }
    }

    // If the current tool does not match the current type, check for
    // a version of the same tool that does
    {
      TTool *tool = this;

      if ((levelType == PLI_XSHLEVEL) && !(targetType & VectorImage))
        tool = TTool::getTool(m_name, VectorImage);
      else if ((levelType == TZP_XSHLEVEL) && !(targetType & ToonzImage))
        tool = TTool::getTool(m_name, ToonzImage);
      else if ((levelType == OVL_XSHLEVEL) && !(targetType & RasterImage))
        tool = TTool::getTool(m_name, RasterImage);
      else if ((levelType == MESH_XSHLEVEL) && !(targetType & MeshImage))
        tool = TTool::getTool(m_name, MeshImage);

      if (tool && tool != this && tool->getTargetType() != TTool::NoTarget)
        return tool->updateEnabled();
    }
  }

  bool spline = m_application->getCurrentObject()->isSpline();

  bool filmstrip = m_application->getCurrentFrame()->isEditingLevel();

  /*-- MultiLayerStylePickerONのときは、現状に関わらず使用可能 --*/
  if (m_name == T_StylePicker &&
      Preferences::instance()->isMultiLayerStylePickerEnabled())
    return (enable(true), QString());

  // Check against camera column
  if (!filmstrip && columnIndex < 0 && (targetType & TTool::EmptyTarget) &&
      (m_name == T_Type || m_name == T_Geometric || m_name == T_Brush))
    return (enable(false), QString());

  // In case of Animate Tool
  if (m_name == T_Edit && !filmstrip) {
    // if an object other than column is selected, then enable the tool
    // regardless of the current column state
    if (!m_application->getCurrentObject()->getObjectId().isColumn())
      return (enable(true), QString());
    // if a column object is selected, switch the inspected column to it
    column = xsh->getColumn(
        m_application->getCurrentObject()->getObjectId().getIndex());
  }

  // Check against splines
  if (spline && (toolType & TTool::LevelTool)) {
    return (targetType & Splines)
               ? (enable(true), QString())
               : (enable(false), QObject::tr("The current tool cannot be "
                                             "used to edit a motion path."));
  }

  // Check against unplaced columns (not in filmstrip mode)
  if (column && !filmstrip) {
    if (column->isLocked() && m_name != T_Selection)
      return (enable(false), QObject::tr("The current column is locked."));

    else if (!column->isCamstandVisible())
      return (enable(false), QObject::tr("The current column is hidden."));

    else if (column->getSoundColumn())
      return (enable(false),
              QObject::tr("It is not possible to edit the audio column."));

    else if (column->getSoundTextColumn())
      return (
          enable(false),
          QObject::tr(
              "Note columns can only be edited in the xsheet or timeline."));

    if (toolType == TTool::ColumnTool) {
      // Check column target
      if (column->getLevelColumn() && !(targetType & LevelColumns))
        return (
            enable(false),
            QObject::tr("The current tool cannot be used on a Level column."));

      if (column->getMeshColumn() && !(targetType & MeshColumns))
        return (
            enable(false),
            QObject::tr("The current tool cannot be used on a Mesh column."));
    }
  }

  // Check column tools
  if (toolType == TTool::ColumnTool) {
    if (filmstrip)
      return (
          enable(false),
          QObject::tr("The current tool cannot be used in Level Strip mode."));

    if ((!column || column->isEmpty()) && !(targetType & TTool::EmptyTarget))
      return (enable(false), QString());
  }

  // Check LevelRead & LevelWrite tools
  if (toolType & TTool::LevelTool) {
    // Check against empty levels
    if (!xl)
      return (targetType & EmptyTarget) ? (enable(true), QString())
                                        : (enable(false), QString());

    // Check against simple-level-edness
    if (!sl)
      return (enable(false),
              QObject::tr("The current level is not editable."));  // Does it
                                                                   // happen at
                                                                   // all btw?

    // Check against level types
    {
      if ((levelType == PLI_XSHLEVEL) && !(targetType & VectorImage))
        return (
            enable(false),
            QObject::tr("The current tool cannot be used on a Vector Level."));

      if ((levelType == TZP_XSHLEVEL) && !(targetType & ToonzImage))
        return (
            enable(false),
            QObject::tr("The current tool cannot be used on a Toonz Level."));

      if ((levelType == OVL_XSHLEVEL) && !(targetType & RasterImage))
        return (
            enable(false),
            QObject::tr("The current tool cannot be used on a Raster Level."));

      if ((levelType == MESH_XSHLEVEL) && !(targetType & MeshImage))
        return (
            enable(false),
            QObject::tr("The current tool cannot be used on a Mesh Level."));

      if ((levelType == META_XSHLEVEL) && !(targetType & MetaImage))
        return (
            enable(false),
            QObject::tr("The current tool cannot be used on a Assistants (Meta) Level."));
    }

    // Check against impossibly traceable movements on the column
    if ((levelType & LEVELCOLUMN_XSHLEVEL) && !filmstrip && columnIndex >= 0) {
      TStageObject *obj =
          xsh->getStageObject(TStageObjectId::ColumnId(columnIndex));
      // Test for Mesh-deformed levels
      const TStageObjectId &parentId = obj->getParent();
      if (parentId.isColumn() && obj->getParentHandle()[0] != 'H') {
        TXshSimpleLevel *parentSl =
            xsh->getCell(rowIndex, parentId.getIndex()).getSimpleLevel();
        if (parentSl && parentSl->getType() == MESH_XSHLEVEL &&
            m_name != T_Selection)
          return (
              enable(false),
              QObject::tr(
                  "The current tool cannot be used on a mesh-deformed level"));
      }
    }

    // Check TTool::ImageType tools
    if (toolType == TTool::LevelWriteTool && m_name != T_Selection) {
      // Check level against read-only status
      if (sl->isFrameReadOnly(getCurrentFid()))
        return (enable(false),
                QObject::tr(
                    "The current frame is locked: any editing is forbidden."));

      // Check level type write support
      if (sl->getPath().getType() ==
              "psd" ||  // We don't have the API to write psd files
          sl->getPath().getType() == "gif" ||
          sl->getPath().getType() == "mp4" ||
          sl->getPath().getType() == "webm" ||
          sl->is16BitChannelLevel() ||  // Inherited by previous
                                        // implementation.
                                        // Could be fixed?
          sl->getProperties()->getBpp() ==
              1)  // Black & White images. Again, could be fixed?

        return (enable(false),
                QObject::tr("The current level is not editable."));
    }
  }

  return (enable(true), QString());
}

//-----------------------------------------------------------------------------

void TTool::setSelectedFrames(const std::set<TFrameId> &selectedFrames) {
  m_selectedFrames = selectedFrames;
  onSelectedFramesChanged();
}
