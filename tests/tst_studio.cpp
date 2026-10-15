#include <atomic>
#include <chrono>
#include <cmath>
#include <vector>
#include <cerrno>
#include <QChildEvent>
#include <QElapsedTimer>
#include <QThread>
#include <QThreadPool>
#include <QSemaphore>
#include <QFutureWatcher>
#include <QScopeGuard>
#include <QtConcurrentRun>
#include <QWheelEvent>
#include "DefaultManager.h"
#include "Devices.h"
#include "Portal.h"
#include "PointerActivity.h"
#include "FileManager.h"
#include "FileTransfer.h"
#include <QDrag>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
// The studio, driven by the keyboard, with no screen involved: the real window
// with real key events, asking the real state what happened.

#include "Clipboard.h"
#include "FolderModel.h"
#include "EntryWatcher.h"
#include "CpuBudget.h"
#include <stdexcept>
#include "GraphItem.h"
#include "IndexModel.h"
#include "Names.h"
#include "Open.h"
#include "Paths.h"
#include "Roots.h"
#include "Theme.h"
#include "Verb.h"
#include "UndoJournal.h"
#include "ArchiveJob.h"
#include <archive.h>
#include <archive_entry.h>
#include "TrashModel.h"
#include "Xattr.h"
#include "Views.h"
#include "Workspace.h"

#include <QAccessible>
#include <QClipboard>
#include <QDateTime>
#include <QDir>
#include "Thumbnail.h"
#include "HelperProcess.h"
#include "Preview.h"
#include "../src/preview/Protocol.h"
#include "omafiles_version.h"
#include <QPdfWriter>
#include <signal.h>
#include <sched.h>
#include <QMutex>
#include <QMutexLocker>
#include <QCryptographicHash>
#include <QFileInfo>
#include <QGuiApplication>
#include <QMimeData>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickWindow>
#include <QPointingDevice>
#include <QStyleHints>
#include <QBuffer>
#include <QImageReader>
#include <QtEndian>
#include <QTabletEvent>
#include <QLockFile>
#include <QRegularExpression>
#include <QPainter>
#include <QProcess>
#include <QScopeGuard>
#include <QtQuickTest>
#include <QSettings>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtQml>
#include <QtTest>

#include <cstdio>
#include <fcntl.h>
#include <pwd.h>
#include <sys/stat.h>
#include <sys/inotify.h>
#include <sys/syscall.h>
#include <unistd.h>

using namespace omafiles;

// Observe native registrations without replacing filesystem notifications.
static thread_local QStringList *registeredWatches = nullptr;
static thread_local QHash<QString, uint32_t> *registeredMasks = nullptr;
extern "C" int inotify_add_watch(int fd, const char *path, uint32_t mask)
{
    const int result = static_cast<int>(::syscall(SYS_inotify_add_watch, fd, path, mask));
    const int savedError = errno;
    if (registeredWatches && result >= 0)
        registeredWatches->append(QString::fromLocal8Bit(path));
    if (registeredMasks && result >= 0)
        registeredMasks->insert(QString::fromLocal8Bit(path), mask);
    errno = savedError;
    return result;
}

static constexpr int asyncTimeout = 30000;

// Hold completion signals while native worker computations finish.
class FuturePublicationBarrier : public QObject {
public:
    explicit FuturePublicationBarrier(QObject *owner) : m_owner(owner) {
        owner->installEventFilter(this);
    }
    ~FuturePublicationBarrier() { release(); }
    void release() { if (m_owner) m_owner->removeEventFilter(this); }
    QFutureWatcherBase *worker() const {
        for (const auto &child : m_children)
            if (auto *watcher = qobject_cast<QFutureWatcherBase *>(child.data())) return watcher;
        return nullptr;
    }
    void publish() {
        auto *watcher = worker();
        watcher->blockSignals(false);
        watcher->finished();
        watcher->blockSignals(true);
    }
protected:
    bool eventFilter(QObject *, QEvent *event) override {
        if (event->type() == QEvent::ChildAdded) {
            auto *child = static_cast<QChildEvent *>(event)->child();
            child->blockSignals(true);
            m_children.append(child);
        }
        return false;
    }
private:
    QPointer<QObject> m_owner;
    QList<QPointer<QObject>> m_children;
};

// A pending scene change, independent of the input helper under test.
class ScenePolish : public QQuickItem {
public:
    using QQuickItem::QQuickItem;
    std::function<void()> action;
    bool completed = false;
    void updatePolish() override {
        action();
        completed = true;
    }
};

class InputPressObserver : public QObject {
public:
    const bool *polished = nullptr;
    int presses = 0;
    bool allAfterPolish = true;
    bool eventFilter(QObject *, QEvent *event) override {
        if (event->type() == QEvent::KeyPress || event->type() == QEvent::TouchBegin
            || event->type() == QEvent::TabletPress || event->type() == QEvent::MouseButtonPress) {
            ++presses;
            if (polished) allAfterPolish = allAfterPolish && *polished;
        }
        return false;
    }
};

class DropReceiver : public QQuickItem {
public:
    explicit DropReceiver(QQuickItem *parent) : QQuickItem(parent) {
        setFlag(ItemAcceptsDrops, true);
    }
    bool accepts = false;
    int enters = 0;
    int drops = 0;
    QPointF position;
protected:
    void dragEnterEvent(QDragEnterEvent *event) override {
        ++enters;
        event->setDropAction(Qt::CopyAction);
        event->setAccepted(accepts);
    }
    void dropEvent(QDropEvent *event) override {
        ++drops;
        position = event->position();
        event->setDropAction(Qt::CopyAction);
        event->setAccepted(accepts);
    }
};

static bool hasFocusOutline(QQuickWindow *window, QQuickItem *frame, const QColor &color);

static QVector<Root> declareRoots(const QVector<Root> &table)
{
    QString error;
    if (!makePath(QFileInfo(defaultRootsPath()).absolutePath(), &error)
        || !saveRoots(defaultRootsPath(), table, &error))
        qFatal("Cannot prepare root declarations: %s", qPrintable(error));
    return setRoots(table);
}

// Desktop actions use a recorder installed before main. Utility integration
// runs in private XDG trees and launches only a scratch argument recorder.
static QVector<OpenRequest> &openRequests()
{
    static QVector<OpenRequest> recorded;
    return recorded;
}

static const bool recorderInstalled = [] {
    setLauncher([](const OpenRequest &request, QString *) {
        openRequests().append(request);
        return true;
    });
    return true;
}();

// The same seam again, for the other door.
//
// Half the row menu is a caller of the command line -- `tag add`, `trash`,
// `mv`, `reindex` -- and a test binary that ran those for real would tag,
// move and delete inside its own fixture, and then in whatever directory a
// case forgot to point somewhere else. So nothing here runs `omafiles`
// either: the command lines are recorded and answered as though they worked,
// which is what these cases are about. That the verbs themselves do what they
// say is `tests/test_cli.py`, against the binary that has them.
static QVector<QStringList> &verbRuns()
{
    static QVector<QStringList> recorded;
    return recorded;
}

static const bool verbRecorderInstalled = [] {
    setVerbRunner([](const QStringList &arguments, VerbReceipt done) {
        verbRuns().append(arguments);
        // Answered at once rather than later: a receipt that never arrived
        // would leave the window waiting for one, and the status bar empty.
        done(true, QString(), QString());
    });
    return true;
}();

static const bool fileDragRecorderInstalled = [] {
    setFileDragRunner([](QDrag *) { return Qt::IgnoreAction; });
    return true;
}();

static bool indexModelHasTag(IndexModel *index, const QString &tag)
{
    for (int i = 0; i < index->tagRowCount(); ++i) {
        if (index->tagAt(i) == tag)
            return true;
    }
    return false;
}

// A hyprland config, written where the theme looks for one.
static bool writeLooknfeel(const QString &path, const QString &body)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate))
        return false;
    return file.write(body.toUtf8()) == body.toUtf8().size();
}

// Points the theme at a scratch desktop for the length of one test, and puts
// the real one back afterwards.
struct DesktopEnv
{
    explicit DesktopEnv(const QString &root)
        : config(qgetenv("XDG_CONFIG_HOME")), omarchy(qgetenv("OMARCHY_PATH"))
    {
        qputenv("XDG_CONFIG_HOME", QDir(root).filePath("config").toUtf8());
        qputenv("OMARCHY_PATH", QDir(root).filePath("omarchy").toUtf8());
    }
    ~DesktopEnv()
    {
        qputenv("XDG_CONFIG_HOME", config);
        qputenv("OMARCHY_PATH", omarchy);
    }
    const QByteArray config;
    const QByteArray omarchy;
};

// The shape omarchy ships as the user's override: the whole thing commented
// out, `rounding` included.
static QString commentedLooknfeel()
{
    return QStringLiteral("-- Change the default Omarchy look'n'feel.\n"
                          "-- hl.config({\n"
                          "--   decoration = {\n"
                          "--     -- Use round window corners.\n"
                          "--     rounding = 8,\n"
                          "--   },\n"
                          "-- })\n");
}

static QString looknfeelRounding(int rounding)
{
    return QStringLiteral("hl.config({\n"
                          "  decoration = {\n"
                          "    rounding = %1,\n"
                          "\n"
                          "    shadow = {\n"
                          "      enabled = false,\n"
                          "    },\n"
                          "  },\n"
                          "})\n")
        .arg(rounding);
}

class TestStudio : public QObject
{
    Q_OBJECT

private slots:
    void navigatesPortalChooser_data();
    void navigatesPortalChooser();
    void monitorsDeviceHotplugAndOpensMountedPaths();
    void cancelsDeviceOperationsAndPreservesServiceRefusals();
    void refusesInvalidDeviceProtocol_data();
    void refusesInvalidDeviceProtocol();
    void reportsDeviceHelperStartupAndExitFailures();
    void refusesUnsafeDeviceOpenReplies_data();
    void refusesUnsafeDeviceOpenReplies();
    void roundTripsArchiveFormats_data();
    void roundTripsArchiveFormats();
    void refusesUnsafeArchiveEntries_data();
    void refusesUnsafeArchiveEntries();
    void extractsContainedLinksAndAccessiblePermissions();
    void cancelsArchivePreparationWithoutPublishing();
    void publishesArchivesWithTagsAndUndo();
    void undoesInstalledArchiveTrees();
    void resolvesArchiveConflicts_data();
    void resolvesArchiveConflicts();
    void stopsArchivePublicationAndAppliesConflictPolicy();
    void refusesUnsafeArchiveDestinations();
    void navigatesArchiveControls_data();
    void navigatesArchiveControls();
    void browsesAndRestoresTrashByKeyboardAndPointer();
    void cancelsStaleTrashScans();
    void keepsTrashCursorOnReloadAndRestore();
    void reversesFreshControlGroups_data();
    void reversesFreshControlGroups();
    void opensFreshOverlaysOnTheirFirstControl_data();
    void opensFreshOverlaysOnTheirFirstControl();
    void mirrorsMenuTraversal_data();
    void mirrorsMenuTraversal();
    void preservesHintTargetsOnPointerActivation_data();
    void preservesHintTargetsOnPointerActivation();
    void restoresKeyRoutingAfterTabRoundTrips_data();
    void restoresKeyRoutingAfterTabRoundTrips();
    void mirrorsTabTraversal_data();
    void mirrorsTabTraversal();
    void resolvesTransferConflictsAndUndoesReplacement_data();
    void resolvesTransferConflictsAndUndoesReplacement();
    void traversesConflictControls_data();
    void traversesConflictControls();
    void cancelsAfterCompletedTransfers();
    void scopesConflictDecisionsToOneBatch();
    void refusesUndoOfChangedReplacementBackups();
    void restoresReplacedSymbolicLinks();
    void cleansUnchangedReplacementBackups();
    void undoesCompletedFileOperations();
    void refusesUndoOfChangedTargets();
    void undoesTagEditsAndPartialBatches();
    void offersUndoByKeyboardPaletteAndPointer();
    void preservesTagsWrittenBeforeTheOperationLock();
    void keepsTextReceiptsForIndexMaintenance();
    void undoesEveryTagEdit_data();
    void undoesEveryTagEdit();
    void undoesDirectoryAndSymbolicLinkMoves();
    void refusesUndoAfterIndependentTagAndModeChanges();
    void boundsSessionUndoHistory();
    void refusesUndoWhenTheIndexCannotCommit();
    void refusesUndoOfAnIdenticalReplacement();
    void preservesSuccessfulProcessDiagnostics();
    void keepsCliNotesInWindowReceipts();
    void keepsFileSelectionVisibleAfterResize();
    void keepsSidebarSelectionVisibleInSmallWindows();
    void scrollsActualSheetsWithPageKeys();
    void scrollsLongOverlayListsWithPointerAndKeyboard();
    void keepsTagEditorFocusAndSuggestionsVisible();
    void scrollsBoundedSheetsAndTooltips();
    void routesSidebarKeysByScrollRange_data();
    void routesSidebarKeysByScrollRange();
    void declinesNavigationWithoutScrollRange_data();
    void declinesNavigationWithoutScrollRange();
    void preservesTextSelectionInResultEditors_data();
    void preservesTextSelectionInResultEditors();
    void preservesNativeTextKeysInScrollAncestors_data();
    void preservesNativeTextKeysInScrollAncestors();
    void preservesModifiedKeysInPreviewAndProperties_data();
    void preservesModifiedKeysInPreviewAndProperties();
    void retainsNativeNavigationInFittingControls_data();
    void retainsNativeNavigationInFittingControls();
    void routesTooltipKeysOnlyWhenContentOverflows_data();
    void routesTooltipKeysOnlyWhenContentOverflows();
    void createsTheFirstTagFromTheEmptyPanel();
    void completesAndRenamesTagsOnTheMarkedSet();
    void keepsTagRefusalsAndPendingTargetsInTheEditor();
    void keepsTagKeyboardFocusInsideTheEditor();
    void editsTagsFromThePaletteMenuAndPointer();
    void editsTagsWithTouchAndPen();
    void exportsNativeFileDragsWithoutDeletingSources();
    void receivesLocalFileDropsWithConventionalActions();
    void dropsIntoFolderRowsTabsAndOtherPanes();
    void linksClipboardFilesByKeyboard();
    void refusesDropsIntoVirtualViews();
    void mapsDropTargetsAfterTheirRowReflows_data();
    void mapsDropTargetsAfterTheirRowReflows();
    void avoidsDropsOnTargetsDestroyedDuringPolish();
    void refusesVirtualTabDropsWhileTheStripReflows();
    void drainsDropConflictsThatArriveDuringCleanup();
    void rejectsMixedClipboardFileUrls();
    void pickSortRoleTogglesOnSameRole();
    void keepsSingleWorkerDirectoryWorkInline();
    void runsDirectoryChunksWithinWorkerBudget();
    void bindsDirectoryChunksToDistinctCpus();
    void releasesDirectoryCpusAfterFailures();
    void mergesParallelDirectorySorts_data();
    void mergesParallelDirectorySorts();
    void avoidsMergingOrderedDirectoryChunks_data();
    void avoidsMergingOrderedDirectoryChunks();
    void propagatesDirectoryChunkFailures();
    void cancelsPendingDirectoryChunks();
    void matchesParallelDirectoryQueries();
    void skipsPartitionPlanningForSmallDirectoryQueries();
    void coalescesPendingDirectoryNotifications();
    void followsDirectoryEntriesOnDisk();
    void watchesDirectoryEntriesAndFileContents();
    void keepsLargeFolderWorkBounded();
    void preservesMixedEncodingDirectoryNames();
    void preparesDirectoryQueriesWithoutChangingUnicodeMatching();
    void sortsAsciiNamesInQtCaseInsensitiveOrder();
    void narrowsCompletedNameQueries_data();
    void narrowsCompletedNameQueries();
    void invalidatesNarrowingForMetadataAndStorage();
    void keepsCanceledQueriesOutOfNarrowingContext();
    void narrowsUnicodeQueriesWithoutSplittingSurrogates();
    void boundsDirectoryNameStorage();
    void reservesParentRowWithoutGrowingTheDirectoryIndex();
    void reusesDirectoryNamesWhenOnlyTheIndexChanges();
    void countsDirectoryScansAcrossPresencePublication_data();
    void countsDirectoryScansAcrossPresencePublication();
    void createsMenuEntriesOnlyWhileOpen();
    void observesPointerMotionOncePerWindow();
    void opensTooltipsOnlyAfterPointerMotion();
    void createsSheetControlsOnlyOnDemand();
    void suspendsHiddenOverlayFocusTracking();
    void retiresDeletedControlsBeforeInspectingTheWindow();
    void cancelsDirectoryWorkAndClearsNavigationState();
    void keepsEveryKeyboardMoveInTheViewport();
    void sharesValidFreedesktopThumbnails();
    void avoidsSourceDecodersForEmptyFiles();
    void boundsThumbnailPixelsToTheDisplayAndRetainsTheSharedCache();
    void invalidatesChangedOrMalformedThumbnails();
    void runsInstalledThumbnailersAndCachesFailures();
    void refusesLinkedThumbnailCachePaths();
    void boundsThumbnailWorkersByCgroupHierarchy_data();
    void boundsThumbnailWorkersByCgroupHierarchy();
    void refusesUnnameableThumbnailControlPaths_data();
    void refusesUnnameableThumbnailControlPaths();
    void restrictsThumbnailWorkersToNativeAffinity();
    void fillsThumbnailSlotsWithinCpuAndMemoryBudgets();
    void assignsThumbnailHelpersToTheirSelectedCpu();
    void preservesInheritedHelperAffinity_data();
    void preservesInheritedHelperAffinity();
    void inheritsPreviewHelperAffinity();
    void cancelsThumbnailWorkAndKeepsTheGuiResponsive();
    void thumbnailsOnlyVisibleRowsInEveryFileView();
    void boundsThumbnailHelpersWithoutCachingExecutionFailures();
    void refreshesVisibleThumbnails_data();
    void refreshesVisibleThumbnails();
    void refreshesSameVersionThumbnailSources();
    void doesNotCacheThumbnailAdmissionLimits();
    void initTestCase();
    void init();
    void cleanupTestCase();

    void walksIntoAFolderAndBackOut();
    void recordsAnOpenInsteadOfLaunchingIt();
    void keepsTheKeyboardAfterEveryStateChange();
    void putsTheCursorBackOnTheFolderItLeft();
    void keepsTheCursorOnTheRowWhileFiltering();
    void readsOutTheRowUnderTheCursor();
    void namesTheRowUnderTheCursorForAssistiveTech();
    void sortsByTagWithTheUntaggedLast();
    void countsWhatIsInAFolder();
    void countsWhatIsTaggedInThisFolder();
    void listsEveryFileCarryingATagWhereverItIs();
    void saysWhetherAFolderIsShutGoneOrEmpty();
    void listsTheNamesTheSystemWillNotDescribe();
    void dropsARowWhoseFolderIsNowAFile();
    void saysWhenNothingInTheIndexCanBeReached();
    void sortsByTypeOnTheSuffix();
    void sortsByModifiedWithinASecond();
    void cyclesTheSortFieldAndItsOwnDirection();
    void showsHiddenFilesOnlyWhenAsked();
    void movesBetweenTheListAndTheTagPanel();
    void reachesControlsWithTabAndComesBackWithEscape();
    void worksTabsAndPanesFromTheKeyboard();
    void typesAPathAndAFilter();
    void walksTheGraphWithTheKeyboard();
    void runsACommandFromThePalette();
    void commandPaletteSortUsesPickSortRole();
    void zoomsAndResetsTheGraph();
    void picksAGraphNodeWithTheMouse();
    void clickingARowFocusesItsPane();
    void drawsEveryView();
    void dressesTheGraphInTheDesktopTheme();
    void paintsTheGraphOnTheThemeBackground();
    void paintsTheFolderOnTheThemeBackground();
    void saysHowMuchOfTheIndexTheGraphDrew();
    void saysWhereTheCursorIsOnAGraphTab();
    void readsTheCornerRoundingFromTheHyprlandConfig();
    void ignoresARoundingInsideALuaComment();
    void roundsNothingWithoutAHyprlandConfig();
    void followsTheDesktopsRoundingIntoQml();
    void showsTheKeySheetAndMarksARow();
    void survivesClosingAPaneWithoutComplaint();
    void noticesTheIndexChangingUnderIt();
    void watchesTheIndexOnAFreshInstall();
    void watchesOwnedStateWithoutWatchingUnrelatedParents_data();
    void watchesOwnedStateWithoutWatchingUnrelatedParents();
    void everyDocumentedKeyIsAnswered();
    void answersTheNamedAndChordedKeys();
    void drawsItself();
    void navigatesDevicesAndTheirActions();
    void opensDeviceMenusByMouseAndTouch();
    void refreshesDeclaredRootsWhenDevicesAppear();
    void watchesDeclaredRootsAcrossCreationAndReplacement();
    void includesQueuedDeclarationRefreshInLoading();
    void refusesPointerInputWhenPolishRetiresTarget_data();
    void refusesPointerInputWhenPolishRetiresTarget();
    void reloadsExternalRootsBeforeReadingIndex();
    void warnsAboutAbsentRootsWithoutDevices_data();
    void warnsAboutAbsentRootsWithoutDevices();
    void keepsStorageWarningsAheadOfCompletedReceipts();
    void expiresTrashReceiptsWithTheirState();
    void prioritizesCurrentStatusAndExpiresDeviceReceipts();
    void expiresReceiptsWhenIndexStateChanges();
    void preservesNewReceiptsDuringStorageRefresh();
    void invalidatesReceiptsAcrossContextsAndActiveOperations();
    void publishesReceiptsAfterUpdatingOperationState();
    void refreshesStoragePresenceOffTheGuiThreadAndCancelsOldWork();
    void refreshesStoragePresenceOffTheGuiThreadAndCancelsOldWork_data();
    void followsEveryThemeChangeAndNotJustTheFirst();
    void keepsMarksInTheTabTheyWereMadeIn();
    void followsTheFolderOnDisk();
    void countsEveryNameAFolderHolds();
    void writesTheDocumentationShots();
    void tagStripSaysHowManyTagsDoNotFit();
    void namesTheMenuRowTheOverflowAndTheTabClose();
    void reachesThePathBarWithTab();
    void pansTheGraphWithShiftHjk();
    void resizesTheSplitWithCtrlComma();
    void runsTheCommandLineForWhatTheRowMenuChanges();
    void actsOnTheMarkedSetOnlyFromInsideIt();
    void copiesWhatTheRowIsToTheClipboard();
    void walksToTheFolderHoldingATaggedFile();
    void marksBySameTagSameTypeAndTheOtherWayRound();
    void worksTheTabMenu();
    void offersTheApplicationsRegisteredForTheFile();
    void walksTagsAsIfTheyWereFolders();
    void asksOneQuestionWhicheverWayItIsWalked();
    void choosesARowMenuEntryWithTheMouse();
    void opensAndSavesAView();
    void walksIntoWhatCarriesNoTag();
    void renamesAndMakesAFolderFromThePromptBar();
    void copiesCutsAndPastesThroughTheDesktopClipboard();
    void marksARunARangeAndAPattern();
    void findsByNameAcrossTheRoots();
    void matchesAPatternTheWayTheCliDoes();
    void runsAScriptOverTheMarkedSet();
    void makesLinksBesideAndHere();
    void listsWhatTheDesktopOpenedLately();
    void choosesTheDetailsColumnsAndKeepsThem();
    void marksWhatARubberBandCrosses();
    void changesPermissionsThroughTheCommandLine();
    void narrowsAnyListingByTypeTagAndDate();
    void trashesAMarkedLinkByItsName();
    void movesAMarkedLinkByItsName();
    void changesPermissionsOfAMarkedLinkByItsName();
    void copiesAndTagsAMarkedLinkThroughItsName();
    void searchesOnlyTheDirectoriesItCanSpell();
    void countsARowUnderAFileAsGone();
    void plansNothingUnderARootThatIsAway();
    void readsNoTagsAtALinkWhereARowWas();
    void listsARecasedNameUnderItsRow();
    void resolvesDesktopLocations();
    void revealsDesktopItemsAndProperties();
    void exportsFileManagerOnAPrivateBus();
    void makesTheFileManagerDefaultFromTheKeyboard();
    void reportsDefaultRegistrationFailures();
    void refusesLinkedDefaultRegistrationPaths();
    void restoresDefaultRegistrationsWithTheirModes();
    void recognizesGeneratedDefaultEntries();
    void refusesUnquotableDefaultExecutables();
    void presentsWithTheCallersActivationToken();
    void restoresInterruptedOverlays_data();
    void restoresInterruptedOverlays();
    void restoresTheTopOverlayWhenOthersClose();
    void stacksAsynchronousReceipts();
    void preservesFocusInCommandsOpenedByOverlays();
    void restoresAvailableOverlayFocusTargets();
    void preservesInterruptedTextSelections_data();
    void preservesInterruptedTextSelections();
    void containsInlinePromptFocus_data();
    void containsInlinePromptFocus();
    void returnsInlinePromptsToTheList();
    void restoresInlinePromptFocusTargets();
    void traversesEveryMenuAction_data();
    void traversesEveryMenuAction();
    void traversesEveryPermissionControl_data();
    void traversesEveryPermissionControl();
    void traversesSidebarActions_data();
    void traversesSidebarActions();
    void auditsPreviewButtons_data();
    void auditsPreviewButtons();
    void auditsInlinePromptButtons_data();
    void auditsInlinePromptButtons();
    void auditsContextualFooterControls();
    void traversesHelpViewport();
    void auditsTagEditorControls_data();
    void auditsTagEditorControls();
    void reportsPreviewFailureReasons_data();
    void reportsPreviewFailureReasons();
    void opensRowMenusByTouch_data();
    void opensRowMenusByTouch();
    void opensSidebarMenusByTouchAndKeyboard();
    void opensRowMenusByPen();
    void waitsForLayoutBeforeInjectingPointerInput_data();
    void waitsForLayoutBeforeInjectingPointerInput();
    void activatesFooterHintsByTouchAndKeyboard();
    void keepsEveryFooterActionReachable_data();
    void keepsEveryFooterActionReachable();
    void cancelsRowHoldOnMovement();
    void waitsForDelayedPlaybackAcknowledgement();
    void boundsPreviewImageTransfers_data();
    void boundsPreviewImageTransfers();
    void previewsFilesWithinTheirBudgets();
    void cancelsPreviewWorkAndBoundsHelpers();
    void navigatesQuickPreviewFromEveryFileView();
    void traversesPreviewControlsAndRestoresCoveredFocus();
    void pausesPreviewMediaWhenCovered();
    void traversesReceiptControls_data();
    void traversesReceiptControls();
    void traversesRefusedPermissionSheets_data();
    void traversesRefusedPermissionSheets();
    void selectsFocusedPreviewText_data();
    void selectsFocusedPreviewText();
    void leavesSelectedPreviewText_data();
    void leavesSelectedPreviewText();

private:
    QQuickWindow *window() const;
    QList<QQuickItem *> itemsNamed(const QString &name) const;
    QQuickItem *itemNamedWithin(QQuickItem *parent, const QString &name) const;
    TabState *tab() const;
    QObject *filesRoot() const;
    int cursor() const;
    QString cwd() const;
    int rowOf(const QString &name) const;
    void focusRow(const QString &name);
    QQuickItem *visibleRow(const QString &objectName, int index) const;
    void openInlinePrompt(const QString &mode);
    void focusPermissionApply();
    /// Opens the row menu and chooses an entry, the way a person does: `m`,
    /// then `j` until it is under the cursor, then Enter.
    ///
    /// By label and not by index. Which entries are drawn depends on what the
    /// row under the cursor is, so an index would quietly start choosing a
    /// different command the day an entry above it grew a `when`.
    bool chooseInMenu(const QString &objectName, const QString &label);
    QStringList menuLabels(const QString &objectName) const;
    template<typename Key>
    void key(Key code, Qt::KeyboardModifiers mods = Qt::NoModifier);
    void settle();
    bool cancelPendingFileDrop();
    void writeIndex();
    /// Which set a picture belongs to. Only `Documentation` may write a name
    /// that `docs/img` uses.
    enum class Shot { Diagnostic, Documentation };
    void shot(const QString &name, Shot kind = Shot::Diagnostic);
    void tagDirectory(const QString &path, const QString &tag, bool reload = true);
    /// Writes `path`'s row into the index in place of the one it had: a second
    /// line for one path is a malformed index.
    void putIndexRow(const QString &path, const QStringList &tags);
    /// `a-target.txt`, `link` to it and `plain.txt` in `dir`, open in the tab
    /// with `link` and `plain.txt` marked and the cursor on `link`.
    void markALinkAndAFile(const QDir &dir);

    QTemporaryDir m_home;
    QTemporaryDir m_state;
    QTemporaryDir m_tree;
    QTemporaryDir m_desktop;
    Theme *m_theme = nullptr;
    IndexModel *m_index = nullptr;
    Clipboard *m_clipboard = nullptr;
    Views *m_views = nullptr;
    Workspace *m_workspace = nullptr;
    QQmlApplicationEngine *m_engine = nullptr;
};

// ------------------------------------------------------------------ fixture

// A real index, so the tag panel has something in it. The panel is walked by
// the keyboard, and a test that only ever sees an empty list proves nothing
// about walking it.
void TestStudio::writeIndex()
{
    const QDir data(m_home.path());
    QVERIFY(data.mkpath(QStringLiteral("omafiles")));

    QFile index(data.filePath(QStringLiteral("omafiles/index.jsonl")));
    QVERIFY(index.open(QIODevice::WriteOnly | QIODevice::Text));

    const QDir tree(m_tree.path());
    const struct { const char *file; const char *tags; } rows[] = {
        {"one.txt", "\"notes\",\"work\""},
        {"two.md", "\"notes\""},
        {"three.ods", "\"project\",\"spreadsheet\""},
        {"four.drawio", "\"diagram\",\"project\""},
        {"five.zip", "\"archive\",\"backup\""},
        {"six.md", "\"notes\",\"project\",\"work\""},
        {"seven.txt", "\"backup\""},
    };
    for (const auto &row : rows) {
        index.write(QStringLiteral("{\"path\":\"%1\",\"tags\":[%2]}\n")
                        .arg(tree.filePath(QString::fromLatin1(row.file)),
                             QString::fromLatin1(row.tags))
                        .toUtf8());
    }
}

void TestStudio::initTestCase()
{
    QCOMPARE(QImageReader::allocationLimit(), 32);
    QTest::defaultTryTimeout.store(std::chrono::milliseconds(asyncTimeout), std::memory_order_relaxed);
    Trash::setMountProvider([] { return QStringList{}; });
    setPreviewHelper(QDir(QCoreApplication::applicationDirPath()).absoluteFilePath("../../build/bin/omafiles-preview"));
    QVERIFY(m_home.isValid());
    QVERIFY(m_state.isValid());
    QVERIFY(m_tree.isValid());

    // The index and the theme both read the user's XDG directories. Pointed at
    // temporaries so a test run never sees, and never writes, real tags.
    qputenv("XDG_DATA_HOME", m_home.path().toUtf8());
    qputenv("XDG_STATE_HOME", m_state.path().toUtf8());
    // `~` and the tab's starting directory both come from $HOME. Pointed at the
    // fixture so the home key is a folder this test built rather than whatever
    // is in the home directory of whoever is running it.
    qputenv("HOME", m_tree.path().toUtf8());
    // The corner rounding comes from the hyprland config, so that gets a
    // fixture too -- the desktop this window is drawn against is the one below
    // and never the one the machine is running. Written the way omarchy ships
    // it: an override that is entirely commented out over a default of 0.
    QVERIFY(m_desktop.isValid());
    const QString systemData = m_desktop.filePath("system");
    QVERIFY(QDir().mkpath(systemData));
    QVERIFY(QFile::link("/usr/share/mime", systemData + "/mime"));
    qputenv("XDG_DATA_DIRS", systemData.toUtf8());
    qputenv("XDG_CONFIG_HOME", QDir(m_desktop.path()).filePath("config").toUtf8());
    qputenv("XDG_CACHE_HOME", QDir(m_desktop.path()).filePath("cache").toUtf8());
    setThumbnailRunner([](const QStringList &, const ThumbnailCancellation &, qint64, int) { return false; });
    qputenv("OMARCHY_PATH", QDir(m_desktop.path()).filePath("omarchy").toUtf8());
    QVERIFY(writeLooknfeel(m_desktop.filePath("config/hypr/looknfeel.lua"),
                           commentedLooknfeel()));
    QVERIFY(writeLooknfeel(m_desktop.filePath("omarchy/default/hypr/looknfeel.lua"),
                           looknfeelRounding(0)));

    const QDir tree(m_tree.path());
    QVERIFY(tree.mkpath(QStringLiteral("alpha/inner")));
    // Somewhere for everyDocumentedKeyIsAnswered to work: not the home folder,
    // so `~` has somewhere to go; with a folder in it, so `l` has something to
    // enter; and with enough rows that `j`, `k` and `G` all land differently.
    QVERIFY(tree.mkpath(QStringLiteral("sweep/nested")));
    // The dotfile is named so it matches a `.txt` filter as well: a filter that
    // begins with a dot must not reveal hidden names.
    for (const QString &name : {QStringLiteral("sweep/one.txt"),
                                 QStringLiteral("sweep/two.txt"),
                                 QStringLiteral("sweep/three.txt"),
                                 QStringLiteral("sweep/.hidden.txt")}) {
        QFile file(tree.filePath(name));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("x");
    }
    QVERIFY(tree.mkpath(QStringLiteral("beta")));
    for (const QString &name : {QStringLiteral("one.txt"), QStringLiteral("two.md"),
                                 QStringLiteral("three.ods"), QStringLiteral("four.drawio"),
                                 QStringLiteral("five.zip"), QStringLiteral("six.md"),
                                 QStringLiteral("seven.txt"),
                                 QStringLiteral("plain.txt")}) {   // deliberately untagged
        QFile file(tree.filePath(name));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("x");
    }

    writeIndex();

    // One of each kind, so the panel has something in it: `v` is on the key
    // sheet, and a key the sweep presses over an empty list proves nothing.
    {
        const QDir config(QDir(m_desktop.path()).filePath(QStringLiteral("config")));
        QVERIFY(config.mkpath(QStringLiteral("omafiles")));
        QFile views(config.filePath(QStringLiteral("omafiles/views.jsonl")));
        QVERIFY(views.open(QIODevice::WriteOnly | QIODevice::Text));
        views.write(QStringLiteral("{\"name\":\"the notes\",\"tags\":[\"notes\"]}\n"
                                   "{\"name\":\"alpha\",\"dir\":\"%1\"}\n")
                        .arg(QDir(m_tree.path()).filePath(QStringLiteral("alpha")))
                        .toUtf8());
    }



    // Nothing is registered or injected here: the singletons are made by the
    // engine during `load`, after the environment above, so they see the
    // temporary XDG directories and never the real ones.
    m_engine = new QQmlApplicationEngine(this);
    m_engine->load(QUrl(QStringLiteral("qrc:/qml/Main.qml")));

    QVERIFY2(!m_engine->rootObjects().isEmpty(), "Main.qml did not load");

    // The same instances the window is bound to, not new ones: a test that made
    // its own would be driving a second copy of the state and asserting about a
    // window that never saw it.
    m_theme = m_engine->singletonInstance<Theme *>(QStringLiteral("omafiles"),
                                                    QStringLiteral("Theme"));
    m_index = m_engine->singletonInstance<IndexModel *>(QStringLiteral("omafiles"),
                                                         QStringLiteral("IndexModel"));
    m_workspace = m_engine->singletonInstance<Workspace *>(QStringLiteral("omafiles"),
                                                            QStringLiteral("Workspace"));
    m_clipboard = m_engine->singletonInstance<Clipboard *>(QStringLiteral("omafiles"),
                                                            QStringLiteral("Clipboard"));
    m_views = m_engine->singletonInstance<Views *>(QStringLiteral("omafiles"),
                                                    QStringLiteral("Views"));
    QVERIFY(m_theme && m_index && m_workspace && m_clipboard && m_views);
    QVERIFY(window());
    QVERIFY(QTest::qWaitForWindowExposed(window(), asyncTimeout));
    QVERIFY(itemsNamed("quickPreview").isEmpty());
}

void TestStudio::cleanupTestCase()
{
    delete m_engine;
    m_engine = nullptr;
}

// Each test starts from the same place: one files tab, at the top of the tree,
// unfiltered, list view, keyboard on the list.
void TestStudio::init()
{
    m_index->clearNotice();
    // A test that failed with a menu, a sheet or a prompt open would hand it to
    // the next one, which would then fail for a reason that is not its own.
    QObject *window = m_engine->rootObjects().first();
    for (QQuickItem *item : window->findChildren<QQuickItem *>()) {
        const QByteArray type = item->metaObject()->className();
        const bool overlay = type.startsWith("ContextMenu") || type.startsWith("CommandPalette")
            || type.startsWith("HelpSheet") || type.startsWith("ModeSheet")
            || type.startsWith("TagSheet") || type.startsWith("PreviewSheet");
        if (overlay && item->isVisible())
            QMetaObject::invokeMethod(item, "close");
    }
    window->setProperty("prompt", QString());
    window->setProperty("filtering", false);
    window->setProperty("editingPath", false);
    QVERIFY(!window->property("blocked").toBool());

    // A test that leaves a graph tab open would hand the next one a window with
    // no file list in it, and the failure would land on the wrong test.
    PaneState *pane = m_workspace->paneAtIndex(0);
    QVERIFY(pane);
    while (m_workspace->paneCount() > 1)
        m_workspace->closePane(m_workspace->paneCount() - 1);
    for (int i = pane->tabCount() - 1; i >= 0; --i) {
        if (pane->tabCount() > 1 && pane->tabList().at(i)->kind() != QLatin1String("files"))
            m_workspace->closeTab(0, i);
    }
    while (pane->tabCount() > 1)
        m_workspace->closeTab(0, pane->tabCount() - 1);
    m_workspace->setFocusedPane(0);
    pane->setCurrentIndex(0);
    settle();

    QVERIFY(tab());
    QCOMPARE(tab()->kind(), QStringLiteral("files"));
    tab()->setSelectedTag(QString());
    tab()->setFilter(QString());
    tab()->setCriteria({});
    tab()->setViewMode(QStringLiteral("list"));
    tab()->setShowHidden(false);
    tab()->setSortRole(QStringLiteral("name"));
    tab()->setSortAscending(true);
    tab()->setCwd(m_tree.path());
    tab()->setMarked({});
    settle();

    QMetaObject::invokeMethod(m_engine->rootObjects().first(), "takeFocus");
    settle();
    QVERIFY(filesRoot());
    QMetaObject::invokeMethod(filesRoot(), "setCursor", Q_ARG(QVariant, QVariant(0)));
    settle();
    QCOMPARE(cwd(), m_tree.path());
}

QQuickWindow *TestStudio::window() const
{
    if (!m_engine || m_engine->rootObjects().isEmpty())
        return nullptr;
    return qobject_cast<QQuickWindow *>(m_engine->rootObjects().first());
}

// Everything on screen under a given objectName.
//
// Not QObject::findChildren: a delegate made by a Repeater or a Loader has no
// QObject parent at all -- only a visual one -- so searching the window's
// object tree finds the handful of items written directly in Main.qml and none
// of the ones a model produced. Panes and key-sheet entries are both the second
// kind, so the search has to be down the visual tree.
QList<QQuickItem *> TestStudio::itemsNamed(const QString &name) const
{
    QList<QQuickItem *> found;
    if (!window() || !window()->contentItem())
        return found;
    QList<QQuickItem *> queue{window()->contentItem()};
    while (!queue.isEmpty()) {
        QQuickItem *item = queue.takeFirst();
        if (item->objectName() == name)
            found.append(item);
        queue.append(item->childItems());
    }
    return found;
}

QQuickItem *TestStudio::itemNamedWithin(QQuickItem *parent, const QString &name) const
{
    for (auto *item : itemsNamed(name))
        if (parent->isAncestorOf(item)) return item;
    return nullptr;
}

TabState *TestStudio::tab() const
{
    return m_workspace->currentTab();
}

QObject *TestStudio::filesRoot() const
{
    return m_engine->rootObjects().first()->property("activeFilesRoot").value<QObject *>();
}

int TestStudio::cursor() const
{
    QObject *root = filesRoot();
    return root ? root->property("cursor").toInt() : -1;
}

QString TestStudio::cwd() const
{
    return tab() ? tab()->cwd() : QString();
}

// Tests name the row they mean rather than counting `j` presses to it. Counting
// keystrokes reads like a user but tests like a hazard: the row a given number
// of presses lands on depends on the sort, the filter and where the cursor
// happened to be when the previous test finished.
int TestStudio::rowOf(const QString &name) const
{
    QObject *folder = filesRoot() ? filesRoot()->property("folder").value<QObject *>() : nullptr;
    if (!folder)
        return -1;
    for (int i = 0; i < folder->property("count").toInt(); ++i) {
        QString path;
        QMetaObject::invokeMethod(folder, "pathAt", Q_RETURN_ARG(QString, path), Q_ARG(int, i));
        if (!path.isEmpty()) {
            if (QFileInfo(path).fileName() == name)
                return i;
            continue;
        }
        // A facet row carries no path -- a tag is not a place -- so what it is
        // called is the only thing it can be found by.
        QVariantMap entry;
        QMetaObject::invokeMethod(folder, "entryAt", Q_RETURN_ARG(QVariantMap, entry),
                                  Q_ARG(int, i));
        if (entry.value(QStringLiteral("name")).toString() == name)
            return i;
    }
    return -1;
}

QQuickItem *TestStudio::visibleRow(const QString &objectName, int index) const
{
    for (auto *item : itemsNamed(objectName))
        if (item->isVisible() && item->property("index").toInt() == index) return item;
    return nullptr;
}

void TestStudio::focusRow(const QString &name)
{
    QTRY_VERIFY2(rowOf(name) >= 0, qPrintable(QStringLiteral("no row named %1").arg(name)));
    const int row = rowOf(name);
    QMetaObject::invokeMethod(filesRoot(), "setCursor", Q_ARG(QVariant, QVariant(row)));
    settle();
    QTRY_COMPARE(filesRoot()->property("entry").toMap().value(QStringLiteral("name")).toString(), name);
}

QStringList TestStudio::menuLabels(const QString &objectName) const
{
    QStringList labels;
    const QList<QQuickItem *> found = itemsNamed(objectName);
    if (found.isEmpty())
        return labels;
    const QVariantList shown = found.first()->property("shown").toList();
    for (const QVariant &entry : shown)
        labels.append(entry.toMap().value(QStringLiteral("label")).toString());
    return labels;
}

bool TestStudio::chooseInMenu(const QString &objectName, const QString &label)
{
    const QStringList labels = menuLabels(objectName);
    const int at = labels.indexOf(label);
    if (at < 0)
        return false;
    // Counted from the top, which is where `openAt` puts the cursor. Not
    // forced there first: a menu that answers a pointer nobody is touching
    // would still land on the right entry if it were, and this counting from
    // zero is what fails loudly when the mouse fights the keyboard.
    for (int i = 0; i < at; ++i)
        key('j');
    key(Qt::Key_Return);
    return true;
}

template<typename Key>
void TestStudio::key(Key code, Qt::KeyboardModifiers mods)
{
    QVERIFY(QQuickTest::qWaitForPolish(window(), asyncTimeout));
    QTest::keyClick(window(), code, mods);
    settle();
}

// Observe both the worker publication and the queued QML cursor/metadata refresh.
void TestStudio::settle()
{
    // Deliver Qt's deferred object retirement before inspecting the visual tree.
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QTRY_VERIFY(!m_index || !m_index->loading());
    QTRY_VERIFY(!m_index || !m_index->operationBusy() || m_index->waitingForVerb()
                || itemsNamed("conflictSheet").first()->isVisible());
    const auto completed = [&] {
        if (m_index && m_index->loading()) return false;
        if (tab() && tab()->kind() == QLatin1String("trash")) {
            const auto views = itemsNamed("trashView");
            if (views.isEmpty()) return false;
            auto *model = views.first()->property("model").value<TrashModel *>();
            return model && !model->loading() && !model->busy();
        }
        QObject *root = filesRoot();
        if (!root) return !tab() || tab()->kind() == QLatin1String("graph");
        auto *folder = qobject_cast<FolderModel *>(root->property("folder").value<QObject *>());
        if (!folder) return false;
        const int row = root->property("cursor").toInt();
        const QVariantMap entry = root->property("entry").toMap();
        return !folder->loading()
            && (folder->rowCount() == 0 || (row >= 0 && row < folder->rowCount()))
            && (folder->rowCount() == 0 || !entry.value(QStringLiteral("mime")).toString().isEmpty())
            && entry == folder->entryAt(row);
    };
    for (int i = 0; i < 4; ++i)
        QCoreApplication::processEvents(QEventLoop::AllEvents);
    QTRY_VERIFY2(completed(), "listing or focused row metadata has not been published");
}

// Renders the window to a file when OMAFILES_SHOTS names a directory. Diagnostic
// pictures go in a subdirectory, so only the documentation case can write a name
// the documentation uses.
void TestStudio::shot(const QString &name, Shot kind)
{
    QString dir = QString::fromLocal8Bit(qgetenv("OMAFILES_SHOTS"));
    if (dir.isEmpty())
        return;
    if (kind == Shot::Diagnostic)
        dir += QStringLiteral("/diagnostic");
    QDir().mkpath(dir);

    // Park the pointer on empty background first. Offscreen it starts at the
    // window's top-left corner, which is a toolbar button, and by the second
    // picture it has hovered there long enough for the tooltip to open over
    // the tab strip.
    QTest::mouseMove(window(), QPoint(int(window()->width() * 0.35),
                                      int(window()->height() * 0.85)));

    // Long enough for the colour transitions to land, and for a tooltip asked
    // for earlier to have gone. Without the wait the picture catches every
    // control mid-fade and reads as though the bindings were stale.
    QTest::qWait(320);
    const QImage image = window()->grabWindow();
    QVERIFY(!image.isNull());
    QVERIFY(image.save(dir + QLatin1Char('/') + name + QStringLiteral(".png")));
}

// Appends a row to the index the way the command line would, without telling
// the window. `reload` says whether to hand it the news; the keys that exist to
// fetch it need a row it has not been handed.
void TestStudio::putIndexRow(const QString &path, const QStringList &tags)
{
    QFile index(QDir(m_home.path()).filePath(QStringLiteral("omafiles/index.jsonl")));
    QVERIFY(index.open(QIODevice::ReadOnly | QIODevice::Text));
    QByteArray kept;
    for (const QByteArray &line : index.readAll().split('\n')) {
        if (!line.trimmed().isEmpty()
            && QJsonDocument::fromJson(line).object().value(QStringLiteral("path")) != path)
            kept += line + '\n';
    }
    index.close();
    const QJsonObject row{{QStringLiteral("path"), path},
                          {QStringLiteral("tags"), QJsonArray::fromStringList(tags)}};
    QVERIFY(index.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text));
    index.write(kept + QJsonDocument(row).toJson(QJsonDocument::Compact) + '\n');
}

void TestStudio::tagDirectory(const QString &path, const QString &tag, bool reload)
{
    putIndexRow(path, {tag});
    if (reload)
        m_index->reload();
    settle();
}

void TestStudio::markALinkAndAFile(const QDir &dir)
{
    for (const char *name : {"a-target.txt", "plain.txt"}) {
        QFile made(dir.filePath(QString::fromLatin1(name)));
        QVERIFY(made.open(QIODevice::WriteOnly));
    }
    const QString link = dir.filePath(QStringLiteral("link"));
    if (!QFileInfo(link).isSymLink())
        QVERIFY(QFile::link(dir.filePath(QStringLiteral("a-target.txt")), link));
    tab()->setCwd(dir.path());
    settle();
    focusRow(QStringLiteral("plain.txt"));
    key(Qt::Key_Space);
    focusRow(QStringLiteral("link"));
    key(Qt::Key_Space);
    QCOMPARE(tab()->markedCount(), 2);
    focusRow(QStringLiteral("link"));
}

// -------------------------------------------------------------------- tests

// Go in, come back, and the keyboard still answers.
void TestStudio::walksIntoAFolderAndBackOut()
{
    key('j');
    QVERIFY2(cursor() > 0, "j did not move the cursor off the parent row");

    focusRow(QStringLiteral("alpha"));
    key('l');
    QCOMPARE(cwd(), m_tree.filePath(QStringLiteral("alpha")));

    key('h');
    QCOMPARE(cwd(), m_tree.path());

    // And again: a window whose first `h` works can still be dead to the keys
    // after it.
    focusRow(QStringLiteral("alpha"));
    key('l');
    QCOMPARE(cwd(), m_tree.filePath(QStringLiteral("alpha")));
    key(Qt::Key_Backspace);
    QCOMPARE(cwd(), m_tree.path());
}

// The other half of `l`: on a folder it walks in, and on a file it asks the
// desktop to open the thing, read here from the recording launcher.
void TestStudio::recordsAnOpenInsteadOfLaunchingIt()
{
    QObject *root = m_engine->rootObjects().first();
    const QString file = m_tree.filePath(QStringLiteral("two.md"));
    focusRow(QStringLiteral("two.md"));

    // Enter.
    openRequests().clear();
    key(Qt::Key_Return);
    QCOMPARE(openRequests().size(), 1);
    QVERIFY2(openRequests().at(0).kind == OpenRequest::Open,
             "Enter on a file asked for something other than an open");
    QCOMPARE(openRequests().at(0).path, file);
    // And it stayed where it was: a file is not a folder to walk into.
    QCOMPARE(cwd(), m_tree.path());

    // `l` and Right are the same move by two other spellings, and the key sheet
    // promises all three.
    key('l');
    QCOMPARE(openRequests().size(), 2);
    QCOMPARE(openRequests().at(1).path, file);
    key(Qt::Key_Right);
    QCOMPARE(openRequests().size(), 3);
    QCOMPARE(openRequests().at(2).path, file);

    // The row menu's own Open, which is a different road to the same place: it
    // goes through the row when the cursor is on it and calls the model
    // directly when it is not.
    openRequests().clear();
    key('m');
    QVERIFY2(root->property("blocked").toBool(), "m did not open the row menu");
    key(Qt::Key_Return);                        // Open is the first entry
    QCOMPARE(openRequests().size(), 1);
    QCOMPARE(openRequests().at(0).path, file);
    QCOMPARE(root->property("blocked").toBool(), false);

    // A file node on the graph hands its path straight to the model, with no
    // folder listing in between.
    openRequests().clear();
    m_workspace->addTab(QStringLiteral("graph"));
    settle();
    QObject *graph = root->property("activeGraph").value<QObject *>();
    QVERIFY(graph);
    QString node;
    for (int i = 0; i < 64 && node.isEmpty(); ++i) {
        key('j');
        const QString id = graph->property("selectedId").toString();
        if (id.startsWith(QLatin1String("file:")))
            node = id;
    }
    QVERIFY2(!node.isEmpty(), "the graph has no file node to activate");
    key('l');
    QCOMPARE(openRequests().size(), 1);
    QVERIFY2(openRequests().at(0).kind == OpenRequest::Open,
             "activating a file node on the graph asked for something else");
    QCOMPARE(openRequests().at(0).path, node.mid(5));
    m_workspace->closeCurrentTab();
    settle();

    // Share goes through the same door. There is no key for it and the menu
    // entry only appears where LocalSend is installed, so it is asked for
    // directly -- a seam that caught the opens and let the shares through would
    // be no seam at all.
    openRequests().clear();
    m_index->sharePath(file);
    QCOMPARE(openRequests().size(), 1);
    QVERIFY2(openRequests().at(0).kind == OpenRequest::Share,
             "sharePath did not ask for a share");
    QCOMPARE(openRequests().at(0).path, file);

    // And a folder is not an open at all.
    openRequests().clear();
    focusRow(QStringLiteral("alpha"));
    key(Qt::Key_Return);
    QCOMPARE(cwd(), m_tree.filePath(QStringLiteral("alpha")));
    QVERIFY2(openRequests().isEmpty(), "walking into a folder asked the desktop to open it");
    key('h');
    QCOMPARE(cwd(), m_tree.path());
}

// The window still answers the keyboard after each kind of write to tab state.
void TestStudio::keepsTheKeyboardAfterEveryStateChange()
{
    // Down if there is room, up otherwise: the point is that the key is
    // answered at all, and the cursor is sometimes already on the last row.
    const auto stillMoves = [this](const char *what) {
        const int before = cursor();
        key('j');
        if (cursor() == before)
            key('k');
        QVERIFY2(cursor() != before, what);
    };

    focusRow(QStringLiteral("alpha"));
    key('l');                       // change directory
    stillMoves("dead after entering a folder");

    key('h');                       // change directory back
    stillMoves("dead after leaving a folder");

    key('2');                       // change view mode
    QCOMPARE(tab()->viewMode(), QStringLiteral("details"));
    stillMoves("dead after switching view");

    key('1');
    key('s');                       // change sort
    QCOMPARE(tab()->sortRole(), QStringLiteral("tags"));
    stillMoves("dead after re-sorting");

    const bool wasAscending = tab()->sortAscending();
    key('S');
    QCOMPARE(tab()->sortAscending(), !wasAscending);
    stillMoves("dead after flipping the sort");

    key(Qt::Key_Space);             // change marks
    stillMoves("dead after marking a row");

    key('A');
    QCOMPARE(filesRoot()->property("folder").value<QObject *>()->property("markedCount").toInt(), 0);
    stillMoves("dead after clearing marks");
}

// Walking out of a folder should land where you were, not at the top.
void TestStudio::putsTheCursorBackOnTheFolderItLeft()
{
    focusRow(QStringLiteral("alpha"));
    key('l');
    QCOMPARE(cwd(), m_tree.filePath(QStringLiteral("alpha")));

    key('h');
    QCOMPARE(cwd(), m_tree.path());

    QObject *folder = filesRoot()->property("folder").value<QObject *>();
    QString under;
    QMetaObject::invokeMethod(folder, "pathAt", Q_RETURN_ARG(QString, under),
                              Q_ARG(int, cursor()));
    QCOMPARE(under, m_tree.filePath(QStringLiteral("alpha")));

    // Enter on the `..` row is the same move as `h`, and has to land in the
    // same place: two ways up that disagree is one of them being wrong.
    focusRow(QStringLiteral("alpha"));
    key('l');
    QCOMPARE(cwd(), m_tree.filePath(QStringLiteral("alpha")));
    QMetaObject::invokeMethod(filesRoot(), "setCursor", Q_ARG(QVariant, QVariant(0)));
    settle();
    key(Qt::Key_Return);
    QCOMPARE(cwd(), m_tree.path());
    QMetaObject::invokeMethod(folder, "pathAt", Q_RETURN_ARG(QString, under),
                              Q_ARG(int, cursor()));
    QCOMPARE(under, m_tree.filePath(QStringLiteral("alpha")));
}

// A filter rebuilds the list under the cursor. The row you were on should stay
// under it for as long as the row is still in the list.
void TestStudio::keepsTheCursorOnTheRowWhileFiltering()
{
    QObject *folder = filesRoot()->property("folder").value<QObject *>();
    focusRow(QStringLiteral("one.txt"));

    tab()->setFilter(QStringLiteral("one"));
    settle();

    QString under;
    QMetaObject::invokeMethod(folder, "pathAt", Q_RETURN_ARG(QString, under),
                              Q_ARG(int, cursor()));
    QCOMPARE(under, m_tree.filePath(QStringLiteral("one.txt")));

    tab()->setFilter(QString());
    settle();
}

// The details panel reads one map off the model. If the map is right the panel
// is right; what it looks like is a separate question from what it knows.
void TestStudio::readsOutTheRowUnderTheCursor()
{
    focusRow(QStringLiteral("two.md"));

    const QVariantMap entry = filesRoot()->property("entry").toMap();
    QCOMPARE(entry.value(QStringLiteral("name")).toString(), QStringLiteral("two.md"));
    QCOMPARE(entry.value(QStringLiteral("path")).toString(),
             m_tree.filePath(QStringLiteral("two.md")));
    QCOMPARE(entry.value(QStringLiteral("directory")).toBool(), false);
    QCOMPARE(entry.value(QStringLiteral("mime")).toString(), QStringLiteral("text/markdown"));
    QVERIFY(!entry.value(QStringLiteral("sizeText")).toString().isEmpty());
    QVERIFY(!entry.value(QStringLiteral("mtimeText")).toString().isEmpty());
    QCOMPARE(entry.value(QStringLiteral("tags")).toStringList(),
             QStringList{QStringLiteral("notes")});

    // Whose it is and what they may do, the way `ls -l` spells it. The owner
    // is whoever wrote the fixture, which is whoever is running this.
    const passwd *me = getpwuid(getuid());
    QVERIFY(me);
    QCOMPARE(entry.value(QStringLiteral("owner")).toString(),
             QString::fromLocal8Bit(me->pw_name));
    QCOMPARE(entry.value(QStringLiteral("mine")).toBool(), true);
    QVERIFY(!entry.value(QStringLiteral("group")).toString().isEmpty());
    const QString mode = entry.value(QStringLiteral("mode")).toString();
    QVERIFY2(QRegularExpression(QStringLiteral("^-[rwx-]{9}$")).match(mode).hasMatch(),
             qPrintable(mode));
    QVERIFY2(mode.startsWith(QStringLiteral("-rw")), qPrintable(mode));

    // A folder row reports itself as one rather than as a file of size zero.
    focusRow(QStringLiteral("alpha"));
    QCOMPARE(filesRoot()->property("entry").toMap()
                 .value(QStringLiteral("directory")).toBool(), true);
    QVERIFY(filesRoot()->property("entry").toMap()
                .value(QStringLiteral("mode")).toString().startsWith(QLatin1Char('d')));

    // Marking the row reads out straight away, though nothing here moves the
    // cursor to make the panel re-read it.
    QObject *folder = filesRoot()->property("folder").value<QObject *>();
    QMetaObject::invokeMethod(folder, "clearMarks");
    focusRow(QStringLiteral("two.md"));
    const auto markedNow = [this] {
        return filesRoot()->property("entry").toMap()
            .value(QStringLiteral("marked")).toBool();
    };
    QCOMPARE(markedNow(), false);
    key(Qt::Key_Space);
    QCOMPARE(markedNow(), true);
    key(Qt::Key_Space);
    QCOMPARE(markedNow(), false);
}

static QQuickItem *viewItemAt(QObject *view, int index)
{
    if (!view)
        return nullptr;
    QQuickItem *item = nullptr;
    QMetaObject::invokeMethod(view, "itemAtIndex",
                              Q_RETURN_ARG(QQuickItem *, item),
                              Q_ARG(int, index));
    return item;
}

static QQuickItem *findAccessibleItem(QQuickItem *root, QAccessible::Role role,
                                      const QString &name = QString())
{
    if (!root)
        return nullptr;
    QList<QQuickItem *> queue{root};
    while (!queue.isEmpty()) {
        QQuickItem *item = queue.takeFirst();
        if (QAccessibleInterface *iface = QAccessible::queryAccessibleInterface(item)) {
            if (iface->role() == role
                && (name.isEmpty() || iface->text(QAccessible::Name) == name))
                return item;
        }
        queue.append(item->childItems());
    }
    return nullptr;
}

static QAccessibleInterface *accessibleOn(QQuickItem *item)
{
    return item ? QAccessible::queryAccessibleInterface(item) : nullptr;
}

static bool focusIsInsidePathCrumbs(QQuickWindow *window)
{
    QQuickItem *focus = window ? window->activeFocusItem() : nullptr;
    for (QQuickItem *item = focus; item; item = item->parentItem()) {
        if (item->objectName() == QLatin1String("pathCrumbs"))
            return true;
    }
    return false;
}


void TestStudio::namesTheRowUnderTheCursorForAssistiveTech()
{
    focusRow(QStringLiteral("two.md"));

    QObject *listing = filesRoot()->property("view").value<QObject *>();
    QVERIFY(listing);
    const int rowIndex = listing->property("currentIndex").toInt();
    QQuickItem *rowItem = viewItemAt(listing, rowIndex);
    QVERIFY(rowItem);
    QAccessibleInterface *rowIface = QAccessible::queryAccessibleInterface(rowItem);
    if (!rowIface)
        QSKIP("QAccessible cannot see QML attached Accessible names in offscreen");
    const QString rowName = rowIface->text(QAccessible::Name);
    QVERIFY2(rowName.contains(QStringLiteral("two.md")),
             qPrintable(QStringLiteral("row accessible name was: %1").arg(rowName)));

    const int tagCount = m_index->tagRowCount();
    QVERIFY2(tagCount >= 1, "the fixture index has no tag vocabulary");

    key('t');
    QObject *dock = m_engine->rootObjects().first()->property("activeDock").value<QObject *>();
    QVERIFY(dock);
    QObject *tags = dock->property("tagsList").value<QObject *>();
    QVERIFY(tags);
    const int tagIndex = tags->property("currentIndex").toInt();
    QVERIFY(tagIndex >= 0);
    QQuickItem *tagItem = viewItemAt(tags, tagIndex);
    QVERIFY(tagItem);
    QAccessibleInterface *tagIface = QAccessible::queryAccessibleInterface(tagItem);
    if (!tagIface)
        QSKIP("QAccessible cannot see QML attached Accessible names in offscreen");
    const QString tagName = tagIface->text(QAccessible::Name);
    QCOMPARE(tagName, m_index->tagAt(tagIndex));

    key(Qt::Key_Escape);

    m_workspace->addTab(QStringLiteral("graph"));
    settle();
    QObject *graph = m_engine->rootObjects().first()->property("activeGraph").value<QObject *>();
    QAccessibleInterface *graphIface = QAccessible::queryAccessibleInterface(
        qobject_cast<QQuickItem *>(graph));
    if (!graphIface)
        QSKIP("QAccessible cannot see QML attached Accessible names in offscreen");
    const QString graphName = graphIface->text(QAccessible::Name);
    QCOMPARE(graphName, QStringLiteral("Graph"));
}

// Sorting by tag is the one sort this program has that a file manager does not,
// and the useful half of it is where the untagged land: at the end going up, so
// you can read the tagged ones, and at the top going down, so you can find what
// still needs tagging.
void TestStudio::sortsByTagWithTheUntaggedLast()
{
    // `s` walks the fields in the order the table lays out its columns, and
    // comes back to where it started.
    key('s');
    QCOMPARE(tab()->sortRole(), QStringLiteral("tags"));
    QCOMPARE(tab()->sortAscending(), true);

    const auto fileRows = [this] {
        QObject *folder = filesRoot()->property("folder").value<QObject *>();
        QStringList out;
        for (int i = 0; i < folder->property("count").toInt(); ++i) {
            QString kind;
            QMetaObject::invokeMethod(folder, "kindAt", Q_RETURN_ARG(QString, kind),
                                      Q_ARG(int, i));
            if (kind != QLatin1String("file"))
                continue;
            QString path;
            QMetaObject::invokeMethod(folder, "pathAt", Q_RETURN_ARG(QString, path),
                                      Q_ARG(int, i));
            out << QFileInfo(path).fileName();
        }
        return out;
    };

    QStringList rows = fileRows();
    QVERIFY(rows.size() > 2);
    QCOMPARE(rows.last(), QStringLiteral("plain.txt"));
    QCOMPARE(rows.first(), QStringLiteral("five.zip"));      // #archive sorts first

    key('S');
    QCOMPARE(tab()->sortAscending(), false);
    rows = fileRows();
    QCOMPARE(rows.first(), QStringLiteral("plain.txt"));

    key('s');
    QCOMPARE(tab()->sortRole(), QStringLiteral("type"));
    key('s');
    QCOMPARE(tab()->sortRole(), QStringLiteral("size"));
    key('s');
    QCOMPARE(tab()->sortRole(), QStringLiteral("mtime"));
    key('s');
    QCOMPARE(tab()->sortRole(), QStringLiteral("name"));
}

// The type column carries the suffix, not the mime description: `.drawio` is
// `text/plain` to the mime database, and a column that called every diagram in
// the index "Plain text document" would be losing the distinction the tags are
// drawing.
void TestStudio::sortsByTypeOnTheSuffix()
{
    QObject *folder = filesRoot()->property("folder").value<QObject *>();

    focusRow(QStringLiteral("four.drawio"));
    QVariantMap entry = filesRoot()->property("entry").toMap();
    QCOMPARE(entry.value(QStringLiteral("type")).toString(), QStringLiteral("drawio"));
    QCOMPARE(entry.value(QStringLiteral("mime")).toString(), QStringLiteral("text/plain"));

    focusRow(QStringLiteral("alpha"));
    QCOMPARE(filesRoot()->property("entry").toMap()
                 .value(QStringLiteral("type")).toString(), QStringLiteral("folder"));

    // Sorted by type, the files group by suffix.
    tab()->setSortRole(QStringLiteral("type"));
    tab()->setSortAscending(true);
    settle();

    QStringList types;
    for (int i = 0; i < folder->property("count").toInt(); ++i) {
        QString kind;
        QMetaObject::invokeMethod(folder, "kindAt", Q_RETURN_ARG(QString, kind), Q_ARG(int, i));
        if (kind != QLatin1String("file"))
            continue;
        QVariantMap row;
        QMetaObject::invokeMethod(folder, "entryAt", Q_RETURN_ARG(QVariantMap, row),
                                  Q_ARG(int, i));
        types << row.value(QStringLiteral("type")).toString();
    }
    QVERIFY(types.size() > 3);
    QStringList sorted = types;
    std::sort(sorted.begin(), sorted.end());
    QCOMPARE(types, sorted);

    tab()->setSortRole(QStringLiteral("name"));
    settle();
}

// Two files modified in the same second sort by which came first, not by name.
void TestStudio::sortsByModifiedWithinASecond()
{
    const auto dateIt = [this](const char *name, const QDateTime &when) {
        QFile file(m_tree.filePath(QString::fromLatin1(name)));
        QVERIFY(file.open(QIODevice::ReadWrite));
        QVERIFY(file.setFileTime(when, QFileDevice::FileModificationTime));
    };
    const auto restore = qScopeGuard([&dateIt] {
        dateIt("five.zip", QDateTime::currentDateTime());
        dateIt("plain.txt", QDateTime::currentDateTime());
    });
    const QDateTime second = QDateTime::fromSecsSinceEpoch(978307200);
    dateIt("plain.txt", second.addMSecs(100));
    dateIt("five.zip", second.addMSecs(900));
    tab()->setSortRole(QStringLiteral("mtime"));
    tab()->setSortAscending(true);
    settle();

    QObject *folder = filesRoot()->property("folder").value<QObject *>();
    QMetaObject::invokeMethod(folder, "reload");
    settle();
    QStringList names;
    for (int i = 0; i < folder->property("count").toInt(); ++i) {
        QVariantMap row;
        QMetaObject::invokeMethod(folder, "entryAt", Q_RETURN_ARG(QVariantMap, row),
                                  Q_ARG(int, i));
        names << row.value(QStringLiteral("name")).toString();
    }
    QCOMPARE(names.mid(names.indexOf(QStringLiteral("plain.txt")), 2),
             (QStringList{QStringLiteral("plain.txt"), QStringLiteral("five.zip")}));

    tab()->setSortRole(QStringLiteral("name"));
    settle();
}

// The size column is blank for a folder, and what belongs there is what is in
// it. Counted for the focused row only, so the answer has to come off the same
// map the panel reads.
void TestStudio::countsWhatIsInAFolder()
{
    focusRow(QStringLiteral("alpha"));                       // holds `inner`
    QVariantMap entry = filesRoot()->property("entry").toMap();
    QCOMPARE(entry.value(QStringLiteral("directory")).toBool(), true);
    QCOMPARE(entry.value(QStringLiteral("items")).toInt(), 1);

    focusRow(QStringLiteral("beta"));                        // holds nothing
    entry = filesRoot()->property("entry").toMap();
    QCOMPARE(entry.value(QStringLiteral("items")).toInt(), 0);

    // A file is not counted; the panel shows its size instead.
    focusRow(QStringLiteral("two.md"));
    entry = filesRoot()->property("entry").toMap();
    QCOMPARE(entry.value(QStringLiteral("directory")).toBool(), false);
    QCOMPARE(entry.value(QStringLiteral("items")).toInt(), -1);
}

// The number that ends the confusion between the two scopes on screen: the
// header counts the whole index, and standing in a folder with none of it, that
// reads as a promise about the list in front of you.
void TestStudio::countsWhatIsTaggedInThisFolder()
{
    QObject *folder = filesRoot()->property("folder").value<QObject *>();

    // The fixture tags every file here but `plain.txt`.
    const int here = folder->property("taggedCount").toInt();
    QVERIFY2(here > 0, "the fixture folder has tagged files and this says none");
    QVERIFY2(here < folder->property("count").toInt(),
             "every row counted as tagged, including the folders and plain.txt");

    // A folder with nothing tagged in it says so, while the index still holds
    // plenty.
    tab()->setCwd(m_tree.filePath(QStringLiteral("alpha")));
    settle();
    QCOMPARE(filesRoot()->property("folder").value<QObject *>()
                 ->property("taggedCount").toInt(), 0);
    QVERIFY(m_index->tagRowCount() > 0);

    // A tagged folder counts, and shows its tags, as `omafiles tag add --
    // some-dir` files them.
    tab()->setCwd(m_tree.path());
    settle();
    const int before = filesRoot()->property("folder").value<QObject *>()
                           ->property("taggedCount").toInt();
    tagDirectory(m_tree.filePath(QStringLiteral("beta")), QStringLiteral("shelf"));
    settle();
    QCOMPARE(filesRoot()->property("folder").value<QObject *>()
                 ->property("taggedCount").toInt(), before + 1);
    focusRow(QStringLiteral("beta"));
    QCOMPARE(filesRoot()->property("entry").toMap()
                 .value(QStringLiteral("tags")).toStringList(),
             QStringList{QStringLiteral("shelf")});

    // And the number reaches the status bar, which is the whole reason it
    // exists. Asserting only on the model left the on-screen half unmeasured.
    QObject *status = m_engine->rootObjects().first()
                          ->findChild<QObject *>(QStringLiteral("statusBar"));
    QVERIFY2(status, "the window has no status bar to read");
    QCOMPARE(status->property("tagged").toInt(),
             filesRoot()->property("folder").value<QObject *>()
                 ->property("taggedCount").toInt());
    QVERIFY(status->property("tagged").toInt() > 0);
}

// A tag is not folder-scoped, and every comparable tool says so: Dolphin lists
// `tags:/work` out of Baloo, TMSU mounts `tags/<tag>/`, Tagsistant does the
// same, and Finder's own wording for a tag is "regardless of which folder they
// are in". This program's `omafiles ls --tag` walks the whole index too, so a
// window that could only answer for one directory disagreed with the command
// line beside it about who carries a tag.
void TestStudio::listsEveryFileCarryingATagWhereverItIs()
{
    // The fixture index goes back whatever happens below: this case writes rows
    // for files outside the tree, and one left behind would land on every test
    // after it.
    const auto restore = qScopeGuard([this] {
        writeIndex();

    // One of each kind, so the panel has something in it: `v` is on the key
    // sheet, and a key the sweep presses over an empty list proves nothing.
    {
        const QDir config(QDir(m_desktop.path()).filePath(QStringLiteral("config")));
        QVERIFY(config.mkpath(QStringLiteral("omafiles")));
        QFile views(config.filePath(QStringLiteral("omafiles/views.jsonl")));
        QVERIFY(views.open(QIODevice::WriteOnly | QIODevice::Text));
        views.write(QStringLiteral("{\"name\":\"the notes\",\"tags\":[\"notes\"]}\n"
                                   "{\"name\":\"alpha\",\"dir\":\"%1\"}\n")
                        .arg(QDir(m_tree.path()).filePath(QStringLiteral("alpha")))
                        .toUtf8());
    }


        m_index->reload();
    settle();
    });

    QTemporaryDir elsewhere;
    QVERIFY(elsewhere.isValid());
    const QString kept = QDir(elsewhere.path()).filePath(QStringLiteral("kept.md"));
    {
        QFile file(kept);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("x");
    }
    // The directory carries the tag as well -- `omafiles tag add -- some-dir`
    // works -- so the listing has a folder row to walk into.
    tagDirectory(kept, QStringLiteral("work"), false);
    tagDirectory(elsewhere.path(), QStringLiteral("work"));
    settle();

    // Standing in a folder with nothing tagged in it, so a listing that reads
    // the directory cannot pass this.
    const QString beta = m_tree.filePath(QStringLiteral("beta"));
    tab()->setCwd(beta);
    settle();
    tab()->setSelectedTag(QStringLiteral("work"));
    settle();

    QObject *folder = filesRoot()->property("folder").value<QObject *>();
    QVERIFY(folder);

    // The rows, against the rule `omafiles ls --tag` follows: every index row
    // carrying the tag whose file the system will describe, and nothing else.
    QStringList listed;
    for (int i = 0; i < folder->property("count").toInt(); ++i) {
        QString kind;
        QMetaObject::invokeMethod(folder, "kindAt", Q_RETURN_ARG(QString, kind),
                                  Q_ARG(int, i));
        QVERIFY2(kind != QLatin1String("parent"), "a tag listing offered a `..` row");
        QString path;
        QMetaObject::invokeMethod(folder, "pathAt", Q_RETURN_ARG(QString, path),
                                  Q_ARG(int, i));
        listed.append(path);
    }
    QStringList expected;
    for (const Record &record : m_index->store().records()) {
        if (record.tags.contains(QLatin1String("work"))
            && rowState(record.path, nullptr) == PathState::Present)
            expected.append(record.path);
    }
    listed.sort();
    expected.sort();
    QCOMPARE(listed, expected);
    QVERIFY2(listed.contains(kept),
             "the listing stopped at the folder the tag was picked in");
    QVERIFY2(listed.size() > 2, "this tag has too few files to prove anything");

    // And it is named as a tag everywhere it is named, rather than dressed as a
    // folder the window is not standing in.
    QCOMPARE(tab()->title(), QStringLiteral("#work"));
    const QList<QQuickItem *> crumbs = itemsNamed(QStringLiteral("pathCrumbs"));
    QCOMPARE(crumbs.size(), 1);
    QCOMPARE(crumbs.first()->property("full").toString(), QStringLiteral("#work"));

    // Not a place in the tree: there is nothing above a tag, so the key that
    // walks out of a folder has nothing to do and the tab stays where it is.
    QVERIFY(folder->property("atRoot").toBool());
    key('h');
    QCOMPARE(tab()->selectedTag(), QStringLiteral("work"));
    QCOMPARE(cwd(), beta);

    // The index is what reloads this list, not a directory watch: a row the
    // command line adds shows up with no key pressed.
    const QString later = QDir(elsewhere.path()).filePath(QStringLiteral("later.txt"));
    {
        QFile file(later);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("x");
    }
    tagDirectory(later, QStringLiteral("work"));
    settle();
    QVERIFY2(rowOf(QStringLiteral("later.txt")) >= 0,
             "a row the index gained never reached the tag listing");

    // Walking into a folder asks for that folder, so it lets the tag go.
    focusRow(QFileInfo(elsewhere.path()).fileName());
    key('l');
    QCOMPARE(tab()->selectedTag(), QString());
    QCOMPARE(cwd(), elsewhere.path());

    tab()->setCwd(m_tree.path());
    settle();
}

// Three folders that draw the same list: one nobody may open, one that is not
// there, and one that is simply empty. The status line is what tells them apart,
// and each of the first two has to leave a `..` row to walk out by.
void TestStudio::saysWhetherAFolderIsShutGoneOrEmpty()
{
    QObject *root = m_engine->rootObjects().first();
    QObject *status = root->findChild<QObject *>(QStringLiteral("statusBar"));
    QVERIFY2(status, "the window has no status bar to read");
    const auto folder = [this] {
        return filesRoot()->property("folder").value<QObject *>();
    };
    const auto complaint = [&] { return folder()->property("error").toString(); };

    // Empty and readable says nothing. That is the answer the other two were
    // being confused with, so it has to stay silent for the difference to mean
    // anything.
    tab()->setCwd(m_tree.filePath(QStringLiteral("beta")));
    settle();
    QCOMPARE(folder()->property("count").toInt(), 1);          // `..` and nothing else
    QCOMPARE(complaint(), QString());
    QCOMPARE(status->property("message").toString(), QString());

    QTemporaryDir scratch;
    QVERIFY(scratch.isValid());
    const QString shut = QDir(scratch.path()).filePath(QStringLiteral("shut"));
    QVERIFY(QDir().mkpath(shut));
    QVERIFY(QFile::setPermissions(shut, QFileDevice::Permissions()));
    // Unconditional, so a failed assertion below still leaves a directory the
    // temporary can remove.
    const auto reopen = qScopeGuard([&shut] {
        QFile::setPermissions(shut, QFileDevice::ReadOwner | QFileDevice::WriteOwner
                                        | QFileDevice::ExeOwner);
    });

    tab()->setCwd(shut);
    settle();
    QCOMPARE(cwd(), shut);
    QVERIFY2(complaint().contains(QStringLiteral("Permission denied")),
             "a folder nobody may open drew as an empty one");
    QCOMPARE(status->property("message").toString(), complaint());
    // And out again, which is `h` -- the row list is only ever `..` here.
    QCOMPARE(folder()->property("count").toInt(), 1);
    key('h');
    QCOMPARE(cwd(), scratch.path());

    // Not there at all. A typed path is the only way to reach one, and the tab
    // is where a typed path lands.
    const QString gone = QDir(scratch.path()).filePath(QStringLiteral("gone"));
    tab()->setCwd(gone);
    settle();
    QCOMPARE(cwd(), gone);
    QVERIFY2(complaint().contains(QStringLiteral("path missing")),
             "a folder that is not there drew as an empty one");
    QCOMPARE(status->property("message").toString(), complaint());
    // The way out is a row, not just a shortcut: Enter on `..` has to work from
    // a directory that does not exist, because that is where you are standing.
    QCOMPARE(folder()->property("count").toInt(), 1);
    QMetaObject::invokeMethod(filesRoot(), "setCursor", Q_ARG(QVariant, QVariant(0)));
    settle();
    key(Qt::Key_Return);
    QCOMPARE(cwd(), scratch.path());

    // A name the kernel will not resolve is not taken as resolved: from inside
    // a folder whose parent is shut, a relative name stays under that folder.
    const QString outer = QDir(scratch.path()).filePath(QStringLiteral("outer"));
    QVERIFY(QDir().mkpath(outer + QStringLiteral("/inner/sub")));
    const QString inner = QFileInfo(outer + QStringLiteral("/inner")).canonicalFilePath();
    const QString started = QDir::currentPath();
    QVERIFY(QDir::setCurrent(inner));
    QVERIFY(QFile::setPermissions(outer, QFileDevice::ReadOwner | QFileDevice::WriteOwner));
    const auto restore = qScopeGuard([&] {
        QFile::setPermissions(outer, QFileDevice::ReadOwner | QFileDevice::WriteOwner
                                         | QFileDevice::ExeOwner);
        QDir::setCurrent(started);
    });
    // A relative name is resolved once, by the tab, so nothing the window
    // binds to it has anything to write back, and no QML says otherwise.
    QTest::failOnWarning(QRegularExpression(QStringLiteral("\\.qml:\\d+")));
    tab()->setCwd(QStringLiteral("sub"));
    settle();
    QCOMPARE(cwd(), inner + QStringLiteral("/sub"));
    QVERIFY2(complaint().contains(QStringLiteral("Permission denied")), qPrintable(complaint()));

    tab()->setCwd(m_tree.path());
    settle();
}

// A row whose folder is now a file is gone, by the rule `prune` and `verify`
// keep: the tag listing and the facet listing leave it out and say nothing,
// where a row the system declines to describe would be named.
void TestStudio::dropsARowWhoseFolderIsNowAFile()
{
    const QString index = QDir(m_home.path()).filePath(QStringLiteral("omafiles/index.jsonl"));
    const auto restore = qScopeGuard([this] {
        writeIndex();
        m_index->reload();
    settle();
    });

    QTemporaryDir scratch;
    QVERIFY(scratch.isValid());
    const QString kept = QDir(scratch.path()).filePath(QStringLiteral("kept.txt"));
    const QString flattened = QDir(scratch.path()).filePath(QStringLiteral("flattened"));
    for (const QString &path : {kept, flattened}) {
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("x");
    }
    {
        QFile file(index);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text));
        for (const QString &path : {kept, flattened + QStringLiteral("/inner.txt")}) {
            file.write(QStringLiteral("{\"path\":\"%1\",\"tags\":[\"work\"]}\n")
                           .arg(path).toUtf8());
        }
    }
    m_index->reload();
    settle();
    settle();

    QObject *folder = filesRoot()->property("folder").value<QObject *>();
    QVERIFY(folder);
    const auto check = [&](const char *listing) {
        QVERIFY2(folder->property("error").toString().isEmpty(),
                 qPrintable(QStringLiteral("the %1 named a row that is gone: %2")
                                .arg(QLatin1String(listing),
                                     folder->property("error").toString())));
        QVERIFY2(rowOf(QStringLiteral("kept.txt")) >= 0,
                 qPrintable(QStringLiteral("the %1 lost the row that is there")
                                .arg(QLatin1String(listing))));
        QVERIFY(rowOf(QStringLiteral("inner.txt")) < 0);
    };

    tab()->setSelectedTag(QStringLiteral("work"));
    settle();
    check("tag listing");
    tab()->setSelectedTag(QString());
    settle();

    m_workspace->addTab(QStringLiteral("tags"));
    settle();
    folder = filesRoot()->property("folder").value<QObject *>();
    QVERIFY(folder);
    tab()->setFacets({QStringLiteral("work")});
    settle();
    check("facet listing");
    m_workspace->closeCurrentTab();
    settle();
}

// A dangling symlink, a symlink round to itself and a fifo are all names the
// folder holds, and Qt's own listing drops every one of them, so a folder of six
// things can list one and look complete. A name whose bytes are not valid UTF-8
// is the other half: that one cannot be spelled at all, so it is counted and
// named back mangled rather than listed as a row nothing could act on.
void TestStudio::listsTheNamesTheSystemWillNotDescribe()
{
    QObject *status = m_engine->rootObjects().first()
                          ->findChild<QObject *>(QStringLiteral("statusBar"));
    QVERIFY(status);

    QTemporaryDir scratch;
    QVERIFY(scratch.isValid());
    const QDir odd(scratch.path());
    {
        QFile file(odd.filePath(QStringLiteral("regular.txt")));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("x");
    }
    QVERIFY(QFile::link(QStringLiteral("/nowhere/at/all"),
                        odd.filePath(QStringLiteral("dangling"))));
    QVERIFY(QFile::link(QStringLiteral("loop"), odd.filePath(QStringLiteral("loop"))));
    QCOMPARE(::mkfifo(QFile::encodeName(odd.filePath(QStringLiteral("pipe"))).constData(),
                      0644), 0);
    // Written with open(2): nothing that takes a QString can spell this name.
    const QByteArray unnameable = QFile::encodeName(scratch.path()) + "/bad\xffname.txt";
    const int fd = ::open(unnameable.constData(), O_CREAT | O_WRONLY, 0644);
    QVERIFY(fd >= 0);
    ::close(fd);
    // QDir cannot see this name, so it cannot remove it either. Unlinked here
    // whatever happens below, or the temporary outlives the run.
    const auto sweep = qScopeGuard([&unnameable] { ::unlink(unnameable.constData()); });

    tab()->setCwd(scratch.path());
    settle();
    QObject *folder = filesRoot()->property("folder").value<QObject *>();

    // `..` and four names, one of which is the only regular file here.
    QCOMPARE(folder->property("count").toInt(), 5);
    for (const QString &name : {QStringLiteral("regular.txt"), QStringLiteral("dangling"),
                                 QStringLiteral("loop"), QStringLiteral("pipe")}) {
        QVERIFY2(rowOf(name) >= 0,
                 qPrintable(QStringLiteral("%1 was not listed").arg(name)));
    }

    // What each row says it is. A link that leads nowhere and one that leads
    // round to itself are the same answer: it is a link and there is nothing at
    // the far end.
    const auto typeOfRow = [this](const QString &name) {
        focusRow(name);
        return filesRoot()->property("entry").toMap()
            .value(QStringLiteral("type")).toString();
    };
    QCOMPARE(typeOfRow(QStringLiteral("regular.txt")), QStringLiteral("txt"));
    QCOMPARE(typeOfRow(QStringLiteral("dangling")), QStringLiteral("broken link"));
    QCOMPARE(typeOfRow(QStringLiteral("loop")), QStringLiteral("broken link"));
    QCOMPARE(typeOfRow(QStringLiteral("pipe")), QStringLiteral("special"));

    // The name that is bytes and not text, said rather than stepped over.
    QVERIFY2(folder->property("error").toString().contains(QStringLiteral("not valid UTF-8")),
             "a name the window cannot spell was dropped in silence");
    QCOMPARE(status->property("message").toString(), folder->property("error").toString());

    // Activating a fifo hands the desktop a read that may never return, so it
    // is refused here and said out loud.
    openRequests().clear();
    focusRow(QStringLiteral("pipe"));
    key(Qt::Key_Return);
    QVERIFY2(openRequests().isEmpty(), "a fifo was handed to the desktop's opener");
    QVERIFY2(m_index->notice().contains(QStringLiteral("not a regular file")),
             "activating a fifo did nothing and said nothing");

    // The dangling link does go to the opener, because the opener is what names
    // both ends of it. The launcher is a seam in this binary, so the refusal is
    // asked of the plan the desktop would have been handed.
    OpenPlan plan;
    QString why;
    QVERIFY2(!planOpen(odd.filePath(QStringLiteral("dangling")), &plan, &why),
             "a link to nothing planned an open");
    QVERIFY(why.contains(QStringLiteral("/nowhere/at/all")));

    tab()->setCwd(m_tree.path());
    settle();
}

// An index with rows in it and nothing reachable is not an index with nothing in
// it, and `0 tags · 0 tagged` over an empty tag panel says it is. The counts mean
// what `omafiles ls` and `omafiles tags` mean -- rows the system will describe --
// so a count that came up short has to say so, and the graph has to name the
// repair that state wants rather than `tag add`.
void TestStudio::saysWhenNothingInTheIndexCanBeReached()
{
    QObject *root = m_engine->rootObjects().first();
    QObject *status = root->findChild<QObject *>(QStringLiteral("statusBar"));
    QVERIFY(status);
    const QString index = QDir(m_home.path()).filePath(QStringLiteral("omafiles/index.jsonl"));

    QTemporaryDir scratch;
    QVERIFY(scratch.isValid());
    const QString shut = QDir(scratch.path()).filePath(QStringLiteral("shut"));
    QVERIFY(QDir().mkpath(shut));
    QStringList held;
    for (const QString &name : {QStringLiteral("held.txt"), QStringLiteral("kept.md")}) {
        const QString path = QDir(shut).filePath(name);
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("x");
        held.append(path);
    }
    QVERIFY(QFile::setPermissions(shut, QFileDevice::Permissions()));
    // The chmod and the fixture index both go back whatever happens below. This
    // case writes an index of rows pointing nowhere, and a failed assertion that
    // left one behind would land on every test after it.
    const auto restore = qScopeGuard([&] {
        QFile::setPermissions(shut, QFileDevice::ReadOwner | QFileDevice::WriteOwner
                                        | QFileDevice::ExeOwner);
        writeIndex();

    // One of each kind, so the panel has something in it: `v` is on the key
    // sheet, and a key the sweep presses over an empty list proves nothing.
    {
        const QDir config(QDir(m_desktop.path()).filePath(QStringLiteral("config")));
        QVERIFY(config.mkpath(QStringLiteral("omafiles")));
        QFile views(config.filePath(QStringLiteral("omafiles/views.jsonl")));
        QVERIFY(views.open(QIODevice::WriteOnly | QIODevice::Text));
        views.write(QStringLiteral("{\"name\":\"the notes\",\"tags\":[\"notes\"]}\n"
                                   "{\"name\":\"alpha\",\"dir\":\"%1\"}\n")
                        .arg(QDir(m_tree.path()).filePath(QStringLiteral("alpha")))
                        .toUtf8());
    }


        m_index->reload();
    settle();
    });
    const QStringList vanished = {QDir(scratch.path()).filePath(QStringLiteral("was-here.txt")),
                                  QDir(scratch.path()).filePath(QStringLiteral("also-here.md"))};

    const auto rewriteIndex = [&](const QStringList &paths) {
        QFile file(index);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text));
        for (const QString &path : paths) {
            file.write(QStringLiteral("{\"path\":\"%1\",\"tags\":[\"work\"]}\n")
                           .arg(path).toUtf8());
        }
        file.close();
        m_index->reload();
    settle();
        settle();
    };

    // Rows the system declines to describe. `prune` must not be the advice for
    // these -- the files are there and behind a chmod.
    rewriteIndex(held);
    QCOMPARE(m_index->fileCount(), 0);
    QCOMPARE(m_index->unknownRows(), 2);
    QCOMPARE(m_index->tagRowCount(), 0);
    QVERIFY2(m_index->error().contains(QStringLiteral("not counted")),
             "the header counted nothing and the window said nothing");
    QCOMPARE(status->property("message").toString(), m_index->error());

    // Rows whose files are simply gone, which is the other answer and the one
    // `prune` settles.
    rewriteIndex(vanished);
    QCOMPARE(m_index->fileCount(), 0);
    QCOMPARE(m_index->missingRows(), 2);
    QVERIFY2(m_index->error().contains(QStringLiteral("omafiles prune")),
             "an index of rows pointing nowhere named no repair");

    // And the graph, which prints the advice. Three states, three pictures: an
    // index with nothing in it wants `tag add`, and the other two do not.
    m_workspace->addTab(QStringLiteral("graph"));
    settle();
    const auto graphShot = [this] {
        settle();
        QTest::qWait(60);
        const QImage shot = window()->grabWindow();
        return shot.copy(QRect(20, 110, qMax(10, shot.width() - 320),
                               qMax(10, shot.height() - 170)));
    };
    const QImage allGone = graphShot();
    rewriteIndex(held);
    const QImage allShut = graphShot();
    rewriteIndex(QStringList());
    const QImage nothingTagged = graphShot();

    QVERIFY2(allGone != nothingTagged,
             "an index of rows whose files are gone advised `omafiles tag add`");
    QVERIFY2(allShut != allGone,
             "a row the system will not describe got the advice for one that is gone");

    m_workspace->closeCurrentTab();
    settle();
}

// `s` picks the field and the direction that field is read in -- A to Z for a
// name, biggest first for a size, newest first for a date -- so it does not keep
// the direction `S` set for the field before it. That is a decision and not a
// slip: Dolphin and nnn both do it, and the key sheet says so. This is what
// holds the program to what the sheet says.
void TestStudio::cyclesTheSortFieldAndItsOwnDirection()
{
    QCOMPARE(tab()->sortRole(), QStringLiteral("name"));
    QCOMPARE(tab()->sortAscending(), true);

    key('S');
    QCOMPARE(tab()->sortAscending(), false);

    key('s');                                   // name -> tags, a vocabulary reads A to Z
    QCOMPARE(tab()->sortRole(), QStringLiteral("tags"));
    QVERIFY2(tab()->sortAscending(), "s kept the direction set for the field before it");

    key('s');                                   // tags -> type
    key('s');                                   // type -> size, biggest first
    QCOMPARE(tab()->sortRole(), QStringLiteral("size"));
    QVERIFY2(!tab()->sortAscending(), "s did not start size at the biggest row");

    key('S');
    QCOMPARE(tab()->sortAscending(), true);
    key('s');                                   // size -> modified, newest first
    QCOMPARE(tab()->sortRole(), QStringLiteral("mtime"));
    QVERIFY2(!tab()->sortAscending(), "s kept the direction set for the field before it");
}

// Dotfiles are hidden until a key asks for them, and a filter is only a filter:
// narrowing the list to `.txt` must not also change what the list is allowed to
// hold, or looking for text files turns up every hidden name in the folder and
// nothing says why.
void TestStudio::showsHiddenFilesOnlyWhenAsked()
{
    tab()->setCwd(m_tree.filePath(QStringLiteral("sweep")));
    settle();

    QCOMPARE(tab()->showHidden(), false);
    QVERIFY2(rowOf(QStringLiteral(".hidden.txt")) < 0, "a dotfile was listed unasked");

    // The bare key nnn uses, and the chord GTK, Nautilus, Dolphin and ranger use.
    for (int i = 0; i < 2; ++i) {
        const char *which = i == 0 ? "`.`" : "Ctrl+H";
        const auto press = [this, i] {
            if (i == 0)
                key('.');
            else
                key(Qt::Key_H, Qt::ControlModifier);
        };

        press();
        QVERIFY2(tab()->showHidden(),
                 qPrintable(QStringLiteral("%1 did not ask for the hidden files")
                                .arg(QLatin1String(which))));
        QVERIFY2(rowOf(QStringLiteral(".hidden.txt")) >= 0, "the dotfile is still not listed");

        press();
        QCOMPARE(tab()->showHidden(), false);
        QVERIFY2(rowOf(QStringLiteral(".hidden.txt")) < 0, "the dotfile stayed listed");
    }

    // And a filter that begins with a dot is a filter and nothing else.
    tab()->setFilter(QStringLiteral(".txt"));
    settle();
    QVERIFY2(rowOf(QStringLiteral("one.txt")) >= 0, "the filter dropped a row it matches");
    QVERIFY2(rowOf(QStringLiteral(".hidden.txt")) < 0,
             "a filter beginning with a dot showed the hidden files");
    tab()->setFilter(QString());
    settle();
}

// `t` hands the keyboard to the tag panel and Escape takes it back. Both halves
// matter: a panel you can enter and not leave is worse than one you cannot
// reach at all.
void TestStudio::movesBetweenTheListAndTheTagPanel()
{
    QObject *root = m_engine->rootObjects().first();
    QCOMPARE(root->property("focusRegion").toString(), QStringLiteral("list"));
    const int tagCount = m_index->tagRowCount();
    QVERIFY2(tagCount >= 3, "the fixture index has no tag vocabulary to walk");

    key('t');
    QCOMPARE(root->property("focusRegion").toString(), QStringLiteral("tags"));

    QObject *dock = root->property("activeDock").value<QObject *>();
    QVERIFY2(dock, "the tag panel is not the one the keyboard is talking to");
    QObject *tags = dock->property("tagsList").value<QObject *>();
    QVERIFY(tags);
    QCOMPARE(tags->property("currentIndex").toInt(), 0);

    key('j');
    QCOMPARE(tags->property("currentIndex").toInt(), 1);
    key('k');
    QCOMPARE(tags->property("currentIndex").toInt(), 0);
    key('G');
    QCOMPARE(tags->property("currentIndex").toInt(), tagCount - 1);

    // Enter picks the tag the cursor is on, and picking it again lets it go.
    key(Qt::Key_Return);
    QCOMPARE(tab()->selectedTag(), m_index->tagAt(tagCount - 1));
    key(Qt::Key_Return);
    QCOMPARE(tab()->selectedTag(), QString());

    key(Qt::Key_Escape);
    QCOMPARE(root->property("focusRegion").toString(), QStringLiteral("list"));

    const int before = cursor();
    key('j');
    QVERIFY2(cursor() != before, "the list stopped answering after a trip to the tag panel");

    // The way out of a tag listing is the way in: `t` puts the cursor on the tag
    // the listing is showing, so Enter lets it go. Landing on row 0 instead
    // leaves the keyboard hunting the vocabulary for the tag it is already
    // looking at.
    tab()->setSelectedTag(m_index->tagAt(1));
    settle();
    key('t');
    QCOMPARE(tags->property("currentIndex").toInt(), 1);
    key(Qt::Key_Return);
    QCOMPARE(tab()->selectedTag(), QString());
    key(Qt::Key_Escape);

    // The other two ways out. `h` and Left are what the rest of the window uses
    // for "back", the panel answers both, and nothing pressed either of them --
    // so they could stop working and the panel would still look reachable.
    for (int i = 0; i < 2; ++i) {
        key('t');
        QCOMPARE(root->property("focusRegion").toString(), QStringLiteral("tags"));
        if (i == 0)
            key('h');
        else
            key(Qt::Key_Left);
        QVERIFY2(root->property("focusRegion").toString() == QLatin1String("list"),
                 i == 0 ? "h did not leave the tag panel"
                        : "Left did not leave the tag panel");
        // And it left the list where it was rather than walking up a folder.
        QCOMPARE(cwd(), m_tree.path());
    }

    // Half a page belongs to whichever list the keyboard is in. The chord block
    // runs before any region is consulted, so Ctrl+D and Ctrl+U decided there
    // walk the file list from inside the panel -- a cursor nobody can see move,
    // moving behind the reader's back.
    key('t');
    QCOMPARE(root->property("focusRegion").toString(), QStringLiteral("tags"));
    tags->setProperty("currentIndex", 0);
    const int fileCursor = cursor();
    key(Qt::Key_D, Qt::ControlModifier);
    QVERIFY2(tags->property("currentIndex").toInt() > 0, "Ctrl+D did not walk the tag list");
    QVERIFY2(cursor() == fileCursor, "Ctrl+D in the tag panel moved the file cursor");
    key(Qt::Key_U, Qt::ControlModifier);
    QCOMPARE(tags->property("currentIndex").toInt(), 0);
    QVERIFY2(cursor() == fileCursor, "Ctrl+U in the tag panel moved the file cursor");
    key(Qt::Key_Escape);

    // And the panel answers `t` in a pane too narrow to keep it drawn. The
    // window opens at 1180, one split leaves each pane under the 640 the dock
    // needs beside a list, and the sheet promises `t` with no size on it -- so
    // the size the program starts at is a size where a documented key has to
    // work.
    QMetaObject::invokeMethod(m_workspace, "splitRight", Q_ARG(QString, QStringLiteral("files")));
    settle();
    const QList<QQuickItem *> panes = itemsNamed(QStringLiteral("pane"));
    QCOMPARE(panes.size(), 2);
    QVERIFY2(panes.at(1)->width() < 640,
             "the split panes are wide enough for the dock, so this proves nothing");
    key('t');
    QVERIFY2(root->property("focusRegion").toString() == QLatin1String("tags"),
             "t did nothing in a pane too narrow to draw the tag panel");
    key(Qt::Key_Escape);
    QMetaObject::invokeMethod(m_workspace, "closePane", Q_ARG(int, 1));
    settle();
}

// Tab has to reach the controls, and Escape has to bring the keyboard back from
// wherever Tab left it. This is the whole of what "reachable by keyboard" means
// for the chips along the top.
void TestStudio::reachesControlsWithTabAndComesBackWithEscape()
{
    QObject *root = m_engine->rootObjects().first();
    QQuickItem *before = window()->activeFocusItem();

    QSet<QQuickItem *> visited;
    QQuickItem *reached = nullptr;
    for (int i = 0; i < 8; ++i) {
        key(Qt::Key_Tab);
        QQuickItem *now = window()->activeFocusItem();
        if (now && now != before) {
            visited.insert(now);
            reached = now;
        }
    }
    QVERIFY2(visited.size() >= 3, "Tab did not reach at least three controls");
    QVERIFY2(window()->activeFocusItem() == reached,
             "Tab did not leave the keyboard on a control");

    key(Qt::Key_Escape);

    // Measured on where the focus actually is: `focusRegion` and `j` alone are
    // both satisfied with Escape doing nothing, since a key on a focused chip
    // bubbles up to the window anyway.
    QVERIFY2(window()->activeFocusItem() != reached,
             "Escape left the keyboard on the control Tab had reached");
    QCOMPARE(window()->activeFocusItem(), before);
    QCOMPARE(root->property("focusRegion").toString(), QStringLiteral("list"));

    const int cursorBefore = cursor();
    key('j');
    QVERIFY2(cursor() != cursorBefore, "Escape did not give the keyboard back to the list");
}

// Tabs and panes are the part of the window with no other way in: there is a
// `+` and a context menu for making a tab, and nothing at all on screen for
// moving between panes. If these keys do not work, that half of the studio is
// mouse-only.
void TestStudio::worksTabsAndPanesFromTheKeyboard()
{
    PaneState *pane = m_workspace->paneAtIndex(0);
    QVERIFY(pane);
    QCOMPARE(pane->tabCount(), 1);

    key('t', Qt::ControlModifier);
    QCOMPARE(pane->tabCount(), 2);
    QCOMPARE(pane->currentIndex(), 1);

    key('t', Qt::ControlModifier);
    QCOMPARE(pane->tabCount(), 3);

    key('[');
    QCOMPARE(pane->currentIndex(), 1);
    key(']');
    QCOMPARE(pane->currentIndex(), 2);
    key(']');                                   // wraps
    QCOMPARE(pane->currentIndex(), 0);

    key(Qt::Key_2, Qt::AltModifier);
    QCOMPARE(pane->currentIndex(), 1);

    key(Qt::Key_Tab, Qt::ControlModifier);
    QCOMPARE(pane->currentIndex(), 2);
    key(Qt::Key_Backtab, Qt::ControlModifier | Qt::ShiftModifier);
    QCOMPARE(pane->currentIndex(), 1);

    key('w', Qt::ControlModifier);
    QCOMPARE(pane->tabCount(), 2);

    // Delete on a focused tab handle closes it -- the same action as Ctrl w,
    // for when the keyboard is on the strip rather than the list.
    {
        const int tabsBefore = pane->tabCount();
        QQuickItem *handle = nullptr;
        for (int i = 0; i < 16; ++i) {
            key(Qt::Key_Tab);
            QQuickItem *now = window()->activeFocusItem();
            if (now && now->objectName() == QLatin1String("tabHandle")) {
                handle = now;
                break;
            }
        }
        QVERIFY2(handle, "Tab did not reach a tab handle");
        key(Qt::Key_Delete);
        QCOMPARE(pane->tabCount(), tabsBefore - 1);
    }
    QMetaObject::invokeMethod(m_engine->rootObjects().first(), "takeFocus");
    settle();

    // Panes.
    key(Qt::Key_Backslash, Qt::ControlModifier);
    QCOMPARE(m_workspace->paneCount(), 2);
    QCOMPARE(m_workspace->focusedPane(), 1);

    key('h', Qt::AltModifier);
    QCOMPARE(m_workspace->focusedPane(), 0);
    key('l', Qt::AltModifier);
    QCOMPARE(m_workspace->focusedPane(), 1);

    key('w', Qt::ControlModifier | Qt::ShiftModifier);
    QCOMPARE(m_workspace->paneCount(), 1);

    // Back to one tab, so the next test starts where this one found things.
    while (m_workspace->paneAtIndex(0)->tabCount() > 1)
        m_workspace->closeTab(0, m_workspace->paneAtIndex(0)->tabCount() - 1);
    settle();

    const int before = cursor();
    key('j');
    QVERIFY2(cursor() != before, "the list stopped answering after the tab and pane keys");
}

// The two text boxes, opened and left by key. Escape has to undo and Enter has
// to commit, and both have to hand the keyboard back to the list -- a field you
// can type into and not get out of is the same trap as a panel you cannot
// leave.
void TestStudio::typesAPathAndAFilter()
{
    QObject *root = m_engine->rootObjects().first();

    key('l', Qt::ControlModifier);
    QCOMPARE(root->property("editingPath").toBool(), true);
    key(Qt::Key_Escape);
    QCOMPARE(root->property("editingPath").toBool(), false);
    QCOMPARE(cwd(), m_tree.path());

    // Typed, committed, and the window went there.
    key('l', Qt::ControlModifier);
    QMetaObject::invokeMethod(root, "finishPathEdit", Q_ARG(QVariant, QVariant(true)),
                              Q_ARG(QVariant, QVariant(m_tree.filePath(
                                        QStringLiteral("alpha")))));
    settle();
    QCOMPARE(cwd(), m_tree.filePath(QStringLiteral("alpha")));
    QCOMPARE(root->property("editingPath").toBool(), false);
    key('h');
    QCOMPARE(cwd(), m_tree.path());

    key('/');
    QCOMPARE(root->property("filtering").toBool(), true);
    QCOMPARE(root->property("focusRegion").toString(), QStringLiteral("filter"));

    for (char ch : QByteArray("one"))
        key(ch);
    QCOMPARE(tab()->filter(), QStringLiteral("one"));

    key(Qt::Key_Return);                        // keep it
    QCOMPARE(root->property("filtering").toBool(), false);
    QCOMPARE(tab()->filter(), QStringLiteral("one"));
    QCOMPARE(root->property("focusRegion").toString(), QStringLiteral("list"));

    key('/');
    key(Qt::Key_Escape);                        // clear it
    QCOMPARE(tab()->filter(), QString());
    QCOMPARE(root->property("filtering").toBool(), false);

    const int before = cursor();
    key('j');
    QVERIFY2(cursor() != before, "the list stopped answering after the text boxes");
}

// The graph is a second view of the same index, and the keyboard both moves
// its selection and opens the node under it.
void TestStudio::walksTheGraphWithTheKeyboard()
{
    QObject *root = m_engine->rootObjects().first();
    m_workspace->addTab(QStringLiteral("graph"));
    settle();
    QCOMPARE(tab()->kind(), QStringLiteral("graph"));
    QVERIFY(root->property("activeGraph").value<QObject *>());

    QObject *graph = root->property("activeGraph").value<QObject *>();
    QCOMPARE(graph->property("selectedId").toString(), QString());

    key('j');
    const QString first = graph->property("selectedId").toString();
    QVERIFY2(!first.isEmpty(), "j did not put the selection on a node");

    key('j');
    QVERIFY(graph->property("selectedId").toString() != first);
    key('k');
    QCOMPARE(graph->property("selectedId").toString(), first);

    key('G');
    const QString last = graph->property("selectedId").toString();
    QVERIFY(last != first);
    key('g');
    QCOMPARE(graph->property("selectedId").toString(), first);

    // Activating a tag node lists the files carrying it: a files tab filtered
    // to that tag, which is what picking the same tag in the dock does to the
    // list beside it, and what a tag in the Places panel does in Dolphin or a
    // tag in the sidebar does in Finder. Written onto the graph tab instead, a
    // tag reaches nothing but that tab's title. The file half is
    // `recordsAnOpenInsteadOfLaunchingIt`.
    graph->setProperty("selectedId", QString());
    QString picked;
    for (int i = 0; i < 32 && picked.isEmpty(); ++i) {
        key('j');
        const QString id = graph->property("selectedId").toString();
        if (id.startsWith(QLatin1String("tag")))
            picked = id;
    }
    QVERIFY2(!picked.isEmpty(), "the graph has no tag node to activate");

    key('l');
    // A tags tab and not a filtered folder: the graph is a map of which tags go
    // together, and the tab it opens is the same fact walked into rather than
    // read. The tag it was standing on is the query it starts on.
    QVERIFY2(tab()->kind() == QLatin1String("tags"),
             "activating a tag node did not open a listing for it");
    QCOMPARE(tab()->facets().size(), 1);
    const QString picked_tag = tab()->facets().first();
    QVERIFY2(!picked_tag.isEmpty(), "the listing a tag node opened is filtered by nothing");
    // The graph draws tags as `#work`; the `#` is decoration and must not reach
    // the index, which has no tag by that name.
    QVERIFY2(!picked_tag.startsWith(QLatin1Char('#')),
             "the graph's # decoration leaked into the tag filter");
    QVERIFY(indexModelHasTag(m_index, picked_tag));
    // Filtered by it, not merely labelled with it: every file row carries the
    // tag. Folders are never filtered out, so they are not counted here.
    QObject *listed = filesRoot()->property("folder").value<QObject *>();
    QVERIFY(listed);
    int files = 0;
    for (int i = 0; i < listed->property("count").toInt(); ++i) {
        QString kind;
        QMetaObject::invokeMethod(listed, "kindAt", Q_RETURN_ARG(QString, kind),
                                  Q_ARG(int, i));
        if (kind != QLatin1String("file"))
            continue;
        ++files;
        QString path;
        QMetaObject::invokeMethod(listed, "pathAt", Q_RETURN_ARG(QString, path),
                                  Q_ARG(int, i));
        QVERIFY2(m_index->tagsForPath(path).contains(picked_tag),
                 "the listing a tag node opened holds a file without that tag");
    }
    QVERIFY2(files > 0, "the listing a tag node opened has no file in it to check");

    m_workspace->closeCurrentTab();
    settle();
    QCOMPARE(tab()->kind(), QStringLiteral("graph"));
    graph = root->property("activeGraph").value<QObject *>();
    QVERIFY(graph);

    // The named spellings of the same four moves: the arrows, Home and End.
    graph->setProperty("selectedId", QString());
    key(Qt::Key_Right);
    const QString stepped = graph->property("selectedId").toString();
    QVERIFY2(!stepped.isEmpty(), "Right did not put the selection on a node");
    key(Qt::Key_Right);
    QVERIFY(graph->property("selectedId").toString() != stepped);
    key(Qt::Key_Left);
    QCOMPARE(graph->property("selectedId").toString(), stepped);
    key(Qt::Key_End);
    const QString ended = graph->property("selectedId").toString();
    QVERIFY(ended != stepped);
    key(Qt::Key_Home);
    QVERIFY(graph->property("selectedId").toString() != ended);

    // Menu opens the row menu, the way it does in the list. A file node this
    // time: the menu is the one thing on the graph that is only for files.
    graph->setProperty("selectedId", QString());
    QString file;
    for (int i = 0; i < 32 && file.isEmpty(); ++i) {
        key('j');
        const QString id = graph->property("selectedId").toString();
        if (!id.isEmpty() && !id.startsWith(QLatin1String("tag")))
            file = id;
    }
    QVERIFY2(!file.isEmpty(), "the graph has no file node to open a menu on");
    key(Qt::Key_Menu);
    QVERIFY2(root->property("blocked").toBool(), "Menu opened nothing on the graph");
    key(Qt::Key_Escape);
    settle();

    // Enter activates, like `l`. A tag node again, for the same reason.
    graph->setProperty("selectedId", picked);
    settle();
    key(Qt::Key_Return);
    QCOMPARE(tab()->kind(), QStringLiteral("tags"));
    QCOMPARE(tab()->facets(), QStringList{picked_tag});
    m_workspace->closeCurrentTab();
    settle();

    m_workspace->closeCurrentTab();
    settle();
    QCOMPARE(tab()->kind(), QStringLiteral("files"));
    QVERIFY2(root->property("activeGraph").value<QObject *>() == nullptr,
             "the closed graph tab is still the one the keyboard talks to");
}

// The palette is the way to reach anything that did not earn a key, so a
// palette that opens and does nothing is worse than no palette. Typed, chosen
// and the window did the thing.
void TestStudio::runsACommandFromThePalette()
{
    QObject *root = m_engine->rootObjects().first();
    QCOMPARE(tab()->viewMode(), QStringLiteral("list"));

    key(':');
    QCOMPARE(root->property("blocked").toBool(), true);

    // Spelled with `j` and `k` in it: in the query they are letters, not list
    // navigation.
    for (char ch : QByteArray("jkicons"))
        key(ch);
    // Both letters, and the whole word, arrived.
    QObject *query = root->findChild<QObject *>(QStringLiteral("paletteQuery"));
    QVERIFY2(query, "the palette has no query field to check");
    QCOMPARE(query->property("text").toString(), QStringLiteral("jkicons"));

    // With them typed the filter matches nothing, so clear and ask properly.
    for (int i = 0; i < 7; ++i)
        key(Qt::Key_Backspace);
    for (char ch : QByteArray("icons"))
        key(ch);
    key(Qt::Key_Return);

    QCOMPARE(tab()->viewMode(), QStringLiteral("icons"));
    QCOMPARE(root->property("blocked").toBool(), false);
    QCOMPARE(root->property("focusRegion").toString(), QStringLiteral("list"));

    key('1');
    QCOMPARE(tab()->viewMode(), QStringLiteral("list"));

    // Opened and abandoned changes nothing and gives the keyboard back.
    key('p', Qt::ControlModifier);
    QCOMPARE(root->property("blocked").toBool(), true);
    key(Qt::Key_Escape);
    QCOMPARE(root->property("blocked").toBool(), false);

    const int before = cursor();
    key('j');
    QVERIFY2(cursor() != before, "the list stopped answering after the palette");
}

// Palette sort commands call pickSortRole, so repeating one toggles direction
// instead of leaving the direction the previous field set.
void TestStudio::commandPaletteSortUsesPickSortRole()
{
    QCOMPARE(tab()->sortRole(), QStringLiteral("name"));
    QCOMPARE(tab()->sortAscending(), true);

    key(':');
    for (char ch : QByteArray("sort-mtime"))
        key(ch);
    key(Qt::Key_Return);

    QCOMPARE(tab()->sortRole(), QStringLiteral("mtime"));
    QCOMPARE(tab()->sortAscending(), false);

    key(':');
    for (char ch : QByteArray("sort-mtime"))
        key(ch);
    key(Qt::Key_Return);

    QCOMPARE(tab()->sortRole(), QStringLiteral("mtime"));
    QCOMPARE(tab()->sortAscending(), true);
}

// `0`, `+` and `-` are read back through GraphItem's `scale`.
void TestStudio::zoomsAndResetsTheGraph()
{
    QObject *root = m_engine->rootObjects().first();
    m_workspace->addTab(QStringLiteral("graph"));
    settle();
    QObject *graph = root->property("activeGraph").value<QObject *>();
    QVERIFY(graph);
    QCOMPARE(graph->property("zoomLevel").toReal(), 1.0);

    // The change has to be announced, not only stored: a binding on zoomLevel
    // that never updates is a readout that lies, and nothing in the QML reads
    // it yet, so this is the only thing keeping the signal honest.
    QSignalSpy announced(graph, SIGNAL(viewChanged()));

    key('+');
    QVERIFY2(announced.count() >= 1, "zooming in did not announce the change");
    const qreal bigger = graph->property("zoomLevel").toReal();
    QVERIFY2(bigger > 1.0, "+ did not zoom in");

    key('-');
    QVERIFY2(graph->property("zoomLevel").toReal() < bigger, "- did not zoom out");

    // `=` is the unshifted `+` on most layouts, and has to zoom the same way.
    // Asserted against the value before the press, which the two presses above
    // have already brought back to 1.0.
    const qreal beforeEquals = graph->property("zoomLevel").toReal();
    key('=');
    QVERIFY2(graph->property("zoomLevel").toReal() > beforeEquals, "= did not zoom in");

    key('+');
    key('+');
    QVERIFY(graph->property("zoomLevel").toReal() != 1.0);
    const int announcedBeforeReset = announced.count();
    key('0');
    QCOMPARE(graph->property("zoomLevel").toReal(), 1.0);
    QVERIFY2(announced.count() > announcedBeforeReset,
             "resetting the view did not announce the change");

    // It stops rather than running away: the bounds are what keep a held key
    // from zooming to a blank screen.
    for (int i = 0; i < 40; ++i)
        key('+');
    QVERIFY(graph->property("zoomLevel").toReal() <= 3.0);
    for (int i = 0; i < 80; ++i)
        key('-');
    QVERIFY(graph->property("zoomLevel").toReal() >= 0.4);

    m_workspace->closeCurrentTab();
    settle();
}

// The graph's mouse handling, including `hitNode`, which decides which node a
// click lands on.
void TestStudio::picksAGraphNodeWithTheMouse()
{
    QObject *root = m_engine->rootObjects().first();
    m_workspace->addTab(QStringLiteral("graph"));
    settle();
    QObject *graph = root->property("activeGraph").value<QObject *>();
    QVERIFY(graph);
    auto *item = qobject_cast<QQuickItem *>(graph);
    QVERIFY(item);

    // The layout is computed from the item's size, so a node's position is only
    // meaningful once that size is final. Forcing a frame settles it, so the
    // position read and the position clicked belong to the same layout.
    QTest::qWait(80);
    window()->grabWindow();
    QTRY_VERIFY(item->width() > 100 && item->height() > 100);
    const QSizeF laidOutAt(item->width(), item->height());

    // Walk to a file node and ask it where it is: `m` reports the node's own
    // view coordinates, so the test needs no guess about the layout.
    QString picked;
    for (int i = 0; i < 64 && picked.isEmpty(); ++i) {
        key('j');
        const QString id = graph->property("selectedId").toString();
        if (id.startsWith(QLatin1String("file")))
            picked = id;
    }
    QVERIFY2(!picked.isEmpty(), "the graph has no file node to click on");

    QSignalSpy asked(graph, SIGNAL(nodeMenuRequested(QString, QString, QString,
                                                     qreal, qreal)));
    key('m');
    QVERIFY2(asked.count() == 1, "m reported no position for the selected node");
    const QPointF at(asked.at(0).at(3).toReal(), asked.at(0).at(4).toReal());
    key(Qt::Key_Escape);

    graph->setProperty("selectedId", QString());
    settle();

    // If the item resized between the two, the coordinates describe a layout
    // that is gone -- say that, rather than failing on the click.
    QCOMPARE(QSizeF(item->width(), item->height()), laidOutAt);

    const QPointF scene = item->mapToScene(at);
    QTest::mouseClick(window(), Qt::LeftButton, Qt::NoModifier, scene.toPoint());
    settle();
    QCOMPARE(graph->property("selectedId").toString(), picked);

    // A click on empty space pans without losing the selection.
    const qreal before = graph->property("zoomLevel").toReal();
    QTest::mouseClick(window(), Qt::LeftButton, Qt::NoModifier, QPoint(4, 200));
    settle();
    QCOMPARE(graph->property("selectedId").toString(), picked);
    QCOMPARE(graph->property("zoomLevel").toReal(), before);

    m_workspace->closeCurrentTab();
    settle();
}

// Every view has to survive being drawn, and be the thing that drew: only the
// region the view owns is read, against the same window with the view empty,
// since the chrome around it has colours of its own.
void TestStudio::drawsEveryView()
{
    QObject *root = m_engine->rootObjects().first();

    // The pane's content area: below the header and tab strip, left of the
    // dock, above the status line. Generous margins, so it holds no chrome.
    const auto contentInk = [this] {
        settle();
        QTest::qWait(60);
        const QImage shot = window()->grabWindow();
        const QRect area(20, 110, qMax(10, shot.width() - 300 - 20),
                          qMax(10, shot.height() - 110 - 60));
        QSet<QRgb> seen;
        int marks = 0;
        const QRgb blank = shot.pixel(area.right() - 2, area.bottom() - 2);
        for (int y = area.top(); y < area.bottom(); y += 3) {
            for (int x = area.left(); x < area.right(); x += 3) {
                const QRgb pixel = shot.pixel(x, y);
                seen.insert(pixel);
                if (pixel != blank)
                    ++marks;
            }
        }
        return QPair<int, int>(seen.size(), marks);
    };

    const auto drew = [&](const char *what, int leastColours, int leastMarks) {
        const QPair<int, int> ink = contentInk();
        QVERIFY2(ink.first >= leastColours && ink.second >= leastMarks,
                 qPrintable(QStringLiteral("%1: %2 colours, %3 marked pixels")
                                .arg(QString::fromLatin1(what))
                                .arg(ink.first)
                                .arg(ink.second)));
    };

    focusRow(QStringLiteral("two.md"));
    drew("the list view drew nothing", 4, 400);
    key('2');
    drew("the details view drew nothing", 4, 400);
    key('3');
    drew("the icons view drew nothing", 4, 400);
    key('1');

    // An empty folder is the floor: whatever the chrome contributes to that
    // rectangle, a drawn view has to beat it.
    tab()->setCwd(m_tree.filePath(QStringLiteral("beta")));
    settle();
    const QPair<int, int> bare = contentInk();
    tab()->setCwd(m_tree.path());
    settle();
    const QPair<int, int> full = contentInk();
    QVERIFY2(full.second > bare.second * 2,
             qPrintable(QStringLiteral("a full folder marked %1 pixels, an empty one %2")
                            .arg(full.second).arg(bare.second)));

    m_workspace->addTab(QStringLiteral("graph"));
    settle();
    QObject *graph = root->property("activeGraph").value<QObject *>();
    QVERIFY(graph);
    const QPair<int, int> drawn = contentInk();

    // The graph, against itself with nothing in it. A painter that returned
    // early would land on the same number as an empty index.
    QFile index(QDir(m_home.path()).filePath(QStringLiteral("omafiles/index.jsonl")));
    QVERIFY(index.open(QIODevice::WriteOnly | QIODevice::Truncate));
    index.close();
    m_index->reload();
    settle();
    const QPair<int, int> empty = contentInk();

    QVERIFY2(drawn.second > empty.second + 200,
             qPrintable(QStringLiteral("the graph marked %1 pixels; with no index at "
                                       "all it marked %2")
                            .arg(drawn.second).arg(empty.second)));
    QVERIFY2(drawn.first > empty.first,
             "the graph drew no colour the empty one did not");

    writeIndex();

    // One of each kind, so the panel has something in it: `v` is on the key
    // sheet, and a key the sweep presses over an empty list proves nothing.
    {
        const QDir config(QDir(m_desktop.path()).filePath(QStringLiteral("config")));
        QVERIFY(config.mkpath(QStringLiteral("omafiles")));
        QFile views(config.filePath(QStringLiteral("omafiles/views.jsonl")));
        QVERIFY(views.open(QIODevice::WriteOnly | QIODevice::Text));
        views.write(QStringLiteral("{\"name\":\"the notes\",\"tags\":[\"notes\"]}\n"
                                   "{\"name\":\"alpha\",\"dir\":\"%1\"}\n")
                        .arg(QDir(m_tree.path()).filePath(QStringLiteral("alpha")))
                        .toUtf8());
    }


    m_index->reload();
    settle();
    m_workspace->closeCurrentTab();
    settle();
}

// Clicking a file in the other pane makes that pane the focused one, so the row
// just clicked is the row `j` moves from.
void TestStudio::clickingARowFocusesItsPane()
{
    QMetaObject::invokeMethod(m_workspace, "splitRight",
                              Q_ARG(QString, QStringLiteral("files")));
    settle();
    QCOMPARE(m_workspace->paneCount(), 2);
    QCOMPARE(m_workspace->focusedPane(), 1);

    // Aimed at the pane itself, not at a fraction of the window. The split is
    // laid out on a later frame than the one `settle` pumps, so a hardcoded
    // point could be measured against a layout that has not happened yet.
    QTest::qWait(80);
    window()->grabWindow();
    const auto panes = [this] { return itemsNamed(QStringLiteral("pane")); };
    QTRY_COMPARE(panes().size(), 2);
    QQuickItem *left = nullptr;
    for (QQuickItem *item : panes()) {
        if (!left || item->mapToScene(QPointF(0, 0)).x() < left->mapToScene(QPointF(0, 0)).x())
            left = item;
    }
    QVERIFY(left);
    QTRY_VERIFY2(left->width() > 80 && left->height() > 80,
                 "the split had not been laid out");
    const QPointF onTheLeft = left->mapToScene(QPointF(left->width() * 0.25,
                                                       left->height() * 0.35));
    // Put the cursor somewhere the click is not, so "the click moved it" is a
    // fact about the click.
    QMetaObject::invokeMethod(filesRoot(), "setCursor", Q_ARG(QVariant, QVariant(0)));
    settle();
    QTest::mouseClick(window(), Qt::LeftButton, Qt::NoModifier, onTheLeft.toPoint());
    settle();
    QCOMPARE(m_workspace->focusedPane(), 0);

    // And the click reached the row, not only the pane under it. The pane's
    // catch-all declines the press so it goes on to the list; a catch-all that
    // took the press instead left the focus right and the row unselected, and
    // this test -- the one named for clicking a row -- could not tell.
    QVERIFY(filesRoot());
    QVERIFY2(cursor() != 0, "the click focused the pane but did not select a row");

    // And the keyboard now drives the pane that was clicked.
    const int before = cursor();
    key('j');
    QVERIFY2(cursor() != before, "the keyboard did not follow the click");

    // And empty space inside a pane counts as that pane. The rows carry a tap
    // handler of their own; below the last row there is nothing but the pane,
    // and a click there moves the keyboard to it.
    m_workspace->setFocusedPane(1);
    settle();
    QCOMPARE(m_workspace->focusedPane(), 1);
    m_workspace->setFocusedPane(1);
    settle();
    QCOMPARE(m_workspace->focusedPane(), 1);
    const QPointF onEmptySpace = left->mapToScene(QPointF(left->width() * 0.25,
                                                          left->height() * 0.9));
    QTest::mouseClick(window(), Qt::LeftButton, Qt::NoModifier, onEmptySpace.toPoint());
    settle();
    QCOMPARE(m_workspace->focusedPane(), 0);

    m_workspace->closePane(1);
    settle();
    QCOMPARE(m_workspace->paneCount(), 1);
}

// A palette on disk, the shape omarchy writes one: `colors.toml` under the
// theme the desktop currently points at.
static bool writePalette(const QString &root, const char *background, const char *foreground,
                          const char *accent, const char *muted)
{
    const QString dir = root + QStringLiteral("/omarchy/current/theme");
    if (!QDir().mkpath(dir))
        return false;
    QFile file(QDir(dir).filePath(QStringLiteral("colors.toml")));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text))
        return false;
    file.write(QStringLiteral("mode = \"dark\"\n"
                               "background = \"%1\"\nforeground = \"%2\"\n"
                               "accent = \"%3\"\nred = \"%3\"\nmuted = \"%4\"\n")
                   .arg(QString::fromLatin1(background), QString::fromLatin1(foreground),
                        QString::fromLatin1(accent), QString::fromLatin1(muted))
                   .toUtf8());
    return true;
}

// The state directory for as long as it is in scope, so a `Theme` built inside
// a test reads the palette that test wrote and the window keeps the fixture's.
struct StateEnv {
    explicit StateEnv(const QString &root)
        : previous(qgetenv("XDG_STATE_HOME"))
    {
        qputenv("XDG_STATE_HOME", root.toUtf8());
    }
    ~StateEnv() { qputenv("XDG_STATE_HOME", previous); }
    const QByteArray previous;
};

// The graph wears the desktop's theme like everything else: the property holds
// the theme, and the pixels change with its palette.
void TestStudio::dressesTheGraphInTheDesktopTheme()
{
    QObject *root = m_engine->rootObjects().first();
    m_workspace->addTab(QStringLiteral("graph"));
    settle();

    QObject *graph = root->property("activeGraph").value<QObject *>();
    QVERIFY(graph);

    QObject *dressed = graph->property("theme").value<QObject *>();
    QVERIFY2(dressed, "the graph has no theme, so it draws in hardcoded colours");
    QCOMPARE(dressed, static_cast<QObject *>(m_theme));

    // And it draws in it. Holding the theme and filling every node from a
    // literal is the same picture on every desktop, and the property above
    // cannot tell the difference -- so this paints the map twice, once per
    // palette, and reads the pixels.
    QTemporaryDir desktop;
    QVERIFY(desktop.isValid());
    const StateEnv scope(desktop.path());

    const auto drawGraph = [](Theme *theme, IndexModel *index) {
        GraphItem item;
        item.setWidth(900);
        item.setHeight(600);
        item.setTheme(theme);
        item.setIndex(index);
        QImage canvas(900, 600, QImage::Format_ARGB32_Premultiplied);
        canvas.fill(theme->background());
        QPainter painter(&canvas);
        item.paint(&painter);
        return canvas;
    };

    // On a desktop of one hue every colour on the map is that hue. A node
    // filled from a literal comes out on another channel, and a line drawn in
    // translucent white comes out grey, whatever the desktop is wearing.
    const auto offPalette = [](const QImage &shot, int channel) {
        int wrong = 0;
        for (int y = 0; y < shot.height(); y += 2) {
            for (int x = 0; x < shot.width(); x += 2) {
                const QColor pixel = shot.pixelColor(x, y);
                const int most = qMax(pixel.red(), qMax(pixel.green(), pixel.blue()));
                const int least = qMin(pixel.red(), qMin(pixel.green(), pixel.blue()));
                // Dark enough to be the background rather than a mark on it.
                if (most <= 24)
                    continue;
                if (most - least <= 6 || (channel == 0 ? pixel.red() : pixel.blue()) < most)
                    ++wrong;
            }
        }
        return wrong;
    };

    QVERIFY(writePalette(desktop.path(), "#000000", "#ff6040", "#ff2020", "#a03030"));
    Theme red;
    const QImage onRed = drawGraph(&red, m_index);
    QVERIFY(writePalette(desktop.path(), "#000000", "#4060ff", "#2020ff", "#3040a0"));
    Theme blue;
    const QImage onBlue = drawGraph(&blue, m_index);

    const int redStrays = offPalette(onRed, 0);
    QVERIFY2(redStrays == 0,
             qPrintable(QStringLiteral("%1 pixels of the map are not the red desktop's")
                            .arg(redStrays)));
    const int blueStrays = offPalette(onBlue, 2);
    QVERIFY2(blueStrays == 0,
             qPrintable(QStringLiteral("%1 pixels of the map are not the blue desktop's")
                            .arg(blueStrays)));
    QVERIFY2(onRed != onBlue, "two desktops, one picture");

    // The line it prints when there is nothing to draw is the desktop's too.
    const int emptyStrays = offPalette(drawGraph(&red, nullptr), 0);
    QVERIFY2(emptyStrays == 0,
             qPrintable(QStringLiteral("%1 pixels of the empty map are not the desktop's")
                            .arg(emptyStrays)));

    m_workspace->closeCurrentTab();
    settle();
}


// The map clears to Theme.background: offscreen grabWindow composites a
// transparent clear onto white.
void TestStudio::paintsTheGraphOnTheThemeBackground()
{
    QObject *root = m_engine->rootObjects().first();
    m_workspace->addTab(QStringLiteral("graph"));
    settle();

    QQuickItem *graph = qobject_cast<QQuickItem *>(
        root->property("activeGraph").value<QObject *>());
    QVERIFY(graph);

    QTest::qWait(60);
    const QImage shot = window()->grabWindow();
    if (shot.isNull())
        QSKIP("grabWindow returned a null image");

    const QColor backdrop = m_theme->background();
    const auto sample = [&](const QPoint &local, const char *where) {
        const QPoint pt = graph->mapToScene(local).toPoint();
        const QColor pixel = shot.pixelColor(pt);
        QVERIFY2(!(pixel.red() > 240 && pixel.green() > 240 && pixel.blue() > 240),
                 qPrintable(QStringLiteral("%1: the graph canvas is the offscreen "
                                           "default (white), not the theme")
                                .arg(QString::fromLatin1(where))));
        const int drift = qAbs(pixel.red() - backdrop.red())
                        + qAbs(pixel.green() - backdrop.green())
                        + qAbs(pixel.blue() - backdrop.blue());
        QVERIFY2(drift < 40,
                 qPrintable(QStringLiteral("%1: the graph canvas is the offscreen "
                                           "default (white) rather than the theme "
                                           "(sampled #%2, expected #%3)")
                                .arg(QString::fromLatin1(where))
                                .arg(pixel.name(QColor::HexRgb))
                                .arg(backdrop.name(QColor::HexRgb))));
    };

    sample(QPoint(12, 12), "top-left");
    sample(QPoint(graph->width() - 12, 12), "top-right");

    m_workspace->closeCurrentTab();
    settle();
}

// Qt Rectangle defaults to white; the files views paint transparent on top of
// this pane. Without an explicit fill the list sits as ink on a white hole while
// the chrome follows Theme.background.
void TestStudio::paintsTheFolderOnTheThemeBackground()
{
    key('1');
    settle();

    const QList<QQuickItem *> panes = itemsNamed(QStringLiteral("pane"));
    QVERIFY2(!panes.isEmpty(), "no pane to sample");
    QQuickItem *pane = panes.first();

    QTest::qWait(60);
    const QImage shot = window()->grabWindow();
    if (shot.isNull())
        QSKIP("grabWindow returned a null image");

    const QColor backdrop = m_theme->background();
    const QPoint local(qRound(pane->width() * 0.55), qRound(pane->height() * 0.85));
    const QPoint pt = pane->mapToScene(local).toPoint();
    const QColor pixel = shot.pixelColor(pt);
    const bool pixelIsWhite = pixel.red() > 240 && pixel.green() > 240 && pixel.blue() > 240;
    const bool themeIsWhite = backdrop.red() > 240 && backdrop.green() > 240 && backdrop.blue() > 240;
    QVERIFY2(!pixelIsWhite || themeIsWhite,
             qPrintable(QStringLiteral("the files pane is Qt's default Rectangle "
                                       "white rather than the theme")));
    const int drift = qAbs(pixel.red() - backdrop.red())
                    + qAbs(pixel.green() - backdrop.green())
                    + qAbs(pixel.blue() - backdrop.blue());
    QVERIFY2(drift < 40,
             qPrintable(QStringLiteral("the files pane is Qt's default Rectangle "
                                       "white rather than the theme "
                                       "(sampled #%1, expected #%2)")
                            .arg(pixel.name(QColor::HexRgb))
                            .arg(backdrop.name(QColor::HexRgb))));
}


// A map of part of the index says which part.
//
// The view carries as many files as a ring of names can be read at, and an
// index larger than that is drawn short. Stopping quietly makes the picture a
// claim about a collection it is not showing, so the count of what it drew and
// the count of what there is both go on the status line -- the same rows
// `omafiles tags` answers with.
void TestStudio::saysHowMuchOfTheIndexTheGraphDrew()
{
    QObject *root = m_engine->rootObjects().first();
    m_workspace->addTab(QStringLiteral("graph"));
    settle();
    QObject *graph = root->property("activeGraph").value<QObject *>();
    QVERIFY(graph);

    // The fixture fits, so there is nothing to leave out and nothing to say.
    QCOMPARE(graph->property("summary").toString(), QStringLiteral("7 tags · 7 files"));

    QTemporaryDir many;
    QVERIFY(many.isValid());
    const QDir dir(many.path());
    QFile index(QDir(m_home.path()).filePath(QStringLiteral("omafiles/index.jsonl")));
    QVERIFY(index.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text));
    const int total = 400;
    for (int i = 0; i < total; ++i) {
        const QString path = dir.filePath(QStringLiteral("file%1.txt").arg(i));
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("x");
        file.close();
        index.write(QStringLiteral("{\"path\":\"%1\",\"tags\":[\"many\"]}\n").arg(path).toUtf8());
    }
    index.close();
    m_index->reload();
    settle();
    settle();

    const int drawn = graph->property("nodeCount").toInt() - 1;   // the one tag
    QVERIFY2(drawn >= 300,
             qPrintable(QStringLiteral("the map carries %1 files of %2").arg(drawn).arg(total)));
    QVERIFY2(drawn < total, "nothing was left out, so there is nothing to check");
    QCOMPARE(graph->property("summary").toString(),
             QStringLiteral("1 tags · %1 of %2 files").arg(drawn).arg(total));

    writeIndex();

    // One of each kind, so the panel has something in it: `v` is on the key
    // sheet, and a key the sweep presses over an empty list proves nothing.
    {
        const QDir config(QDir(m_desktop.path()).filePath(QStringLiteral("config")));
        QVERIFY(config.mkpath(QStringLiteral("omafiles")));
        QFile views(config.filePath(QStringLiteral("omafiles/views.jsonl")));
        QVERIFY(views.open(QIODevice::WriteOnly | QIODevice::Text));
        views.write(QStringLiteral("{\"name\":\"the notes\",\"tags\":[\"notes\"]}\n"
                                   "{\"name\":\"alpha\",\"dir\":\"%1\"}\n")
                        .arg(QDir(m_tree.path()).filePath(QStringLiteral("alpha")))
                        .toUtf8());
    }


    m_index->reload();
    settle();
    settle();
    QCOMPARE(graph->property("summary").toString(), QStringLiteral("7 tags · 7 files"));

    m_workspace->closeCurrentTab();
    settle();
}

// The status line on a graph tab is about the graph.
//
// It reads the pane's `FolderModel`, and a graph tab does not have one: the
// counts on the left are then a confident `0/0` and `0 tagged here` about an
// index with seven tagged files in it. A view says where you are in it in the
// terms it has -- the node under the cursor, of the nodes there are.
void TestStudio::saysWhereTheCursorIsOnAGraphTab()
{
    QObject *root = m_engine->rootObjects().first();
    QObject *status = root->findChild<QObject *>(QStringLiteral("statusBar"));
    QVERIFY(status);
    QObject *note = root->findChild<QObject *>(QStringLiteral("statusNote"));
    QVERIFY(note);
    const int inTheFolder = status->property("total").toInt();
    QVERIFY(inTheFolder > 0);

    m_workspace->addTab(QStringLiteral("graph"));
    settle();
    QObject *graph = root->property("activeGraph").value<QObject *>();
    QVERIFY(graph);

    const int nodes = graph->property("nodeCount").toInt();
    QVERIFY(nodes > 0);
    QCOMPARE(status->property("total").toInt(), nodes);
    QCOMPARE(status->property("cursor").toInt(), -1);
    QCOMPARE(note->property("text").toString(), graph->property("summary").toString());

    key('j');
    QCOMPARE(graph->property("selectedRow").toInt(), 0);
    QCOMPARE(status->property("cursor").toInt(), 0);
    key('j');
    QCOMPARE(status->property("cursor").toInt(), 1);

    // And the folder gets its own line back when the folder is what is in front
    // of you.
    m_workspace->closeCurrentTab();
    settle();
    QCOMPARE(status->property("total").toInt(), inTheFolder);
    QCOMPARE(note->property("text").toString(),
             QStringLiteral("%1 tagged here").arg(status->property("tagged").toInt()));
}

// Every corner in the window is the desktop's, and the desktop does not keep
// that number with its colours.
//
// `colors.toml` is colours only. hyprland's `decoration.rounding` is what every
// `Theme.rounding` binding in the window follows, so that is the file the theme
// reads: the user's override first, omarchy's default behind it.
void TestStudio::readsTheCornerRoundingFromTheHyprlandConfig()
{
    QTemporaryDir desktop;
    QVERIFY(desktop.isValid());
    QVERIFY(writeLooknfeel(desktop.filePath("config/hypr/looknfeel.lua"),
                           looknfeelRounding(12)));
    QVERIFY(writeLooknfeel(desktop.filePath("omarchy/default/hypr/looknfeel.lua"),
                           looknfeelRounding(3)));

    const DesktopEnv scope(desktop.path());
    Theme theme;
    QCOMPARE(theme.rounding(), 12);
}

// The override omarchy ships is the whole config commented out, this machine
// included -- so `rounding = 8` is sitting in it as a suggestion, behind a
// `--`, on every desktop that has never been customised. Read naively that is
// a window with round corners on a desktop with square ones.
void TestStudio::ignoresARoundingInsideALuaComment()
{
    QTemporaryDir desktop;
    QVERIFY(desktop.isValid());
    QVERIFY(writeLooknfeel(desktop.filePath("config/hypr/looknfeel.lua"),
                           commentedLooknfeel()));
    QVERIFY(writeLooknfeel(desktop.filePath("omarchy/default/hypr/looknfeel.lua"),
                           looknfeelRounding(3)));

    const DesktopEnv scope(desktop.path());
    Theme theme;
    QVERIFY2(theme.rounding() != 8, "read a rounding out of a Lua comment");
    // And a file that exists but says nothing on the subject falls through to
    // the default rather than to zero.
    QCOMPARE(theme.rounding(), 3);
}

// A desktop that is not omarchy has neither file, which is square corners and
// nothing else.
void TestStudio::roundsNothingWithoutAHyprlandConfig()
{
    QTemporaryDir desktop;
    QVERIFY(desktop.isValid());

    const DesktopEnv scope(desktop.path());
    Theme theme;
    QCOMPARE(theme.rounding(), 0);
}

// And it reaches the window, live.
//
// The getter alone is worth little: `rounding` is an integer with a default, so
// a theme that opened no file at all still answers a plausible number and
// agrees with itself. What is worth asserting is the radius of something on
// screen, before and after the desktop changes its mind.
void TestStudio::followsTheDesktopsRoundingIntoQml()
{
    QObject *root = m_engine->rootObjects().first();
    const QString looknfeel = m_desktop.filePath("config/hypr/looknfeel.lua");

    // The palette's query box, which is the one named item in the window whose
    // frame is drawn with `radius: Theme.rounding`.
    key(':');
    QCOMPARE(root->property("blocked").toBool(), true);
    QQuickItem *query =
        qobject_cast<QQuickItem *>(root->findChild<QObject *>(QStringLiteral("paletteQuery")));
    QVERIFY(query);
    QQuickItem *frame = query->parentItem();
    QVERIFY(frame);
    QCOMPARE(m_theme->rounding(), 0);
    QCOMPARE(frame->property("radius").toInt(), 0);

    QVERIFY(writeLooknfeel(looknfeel, looknfeelRounding(7)));
    QTRY_COMPARE(m_theme->rounding(), 7);
    settle();
    QCOMPARE(frame->property("radius").toInt(), 7);

    // Back to square, so the rest of the suite and the documentation shots see
    // the window the fixture describes.
    QVERIFY(writeLooknfeel(looknfeel, commentedLooknfeel()));
    QTRY_COMPARE(m_theme->rounding(), 0);
    settle();
    QCOMPARE(frame->property("radius").toInt(), 0);

    key(Qt::Key_Escape);
    QCOMPARE(root->property("blocked").toBool(), false);
}

// `?` and `Space`, which the key sweep skips: one opens a sheet that takes the
// keyboard and the other is spelled as a word.
void TestStudio::showsTheKeySheetAndMarksARow()
{
    QObject *root = m_engine->rootObjects().first();

    QCOMPARE(root->property("blocked").toBool(), false);
    key('?');
    QVERIFY2(root->property("blocked").toBool(), "? did not open the key sheet");
    key('j');
    QVERIFY(root->property("blocked").toBool());
    key(Qt::Key_Escape);
    QCOMPARE(root->property("blocked").toBool(), false);

    QObject *folder = filesRoot()->property("folder").value<QObject *>();
    QMetaObject::invokeMethod(folder, "clearMarks");
    QMetaObject::invokeMethod(filesRoot(), "setCursor", Q_ARG(QVariant, QVariant(1)));
    settle();
    QCOMPARE(folder->property("markedCount").toInt(), 0);

    key(Qt::Key_Space);
    QCOMPARE(folder->property("markedCount").toInt(), 1);
    key(Qt::Key_Space);
    QCOMPARE(folder->property("markedCount").toInt(), 0);

    // And in the tag panel, where it toggles the tag under the cursor.
    key('t');
    QCOMPARE(root->property("focusRegion").toString(), QStringLiteral("tags"));
    QCOMPARE(tab()->selectedTag(), QString());
    key(Qt::Key_Space);
    QVERIFY2(!tab()->selectedTag().isEmpty(), "Space picked no tag in the tag panel");
    key(Qt::Key_Space);
    QCOMPARE(tab()->selectedTag(), QString());
    key(Qt::Key_Escape);
    settle();

    // Every entry on the sheet has to be readable: nothing falls off the
    // bottom and no label is cut off at the right.
    key('?');
    QVERIFY(root->property("blocked").toBool());
    window()->grabWindow();
    settle();
    const QList<QQuickItem *> entries = itemsNamed(QStringLiteral("helpEntry"));
    QVERIFY2(entries.size() >= 20, "the key sheet rendered almost nothing");
    for (QQuickItem *entry : entries) {
        const QString text = entry->property("text").toString();
        QVERIFY2(!entry->property("truncated").toBool(),
                 qPrintable(QStringLiteral("the key sheet cuts off `%1`").arg(text)));
        // `truncated` is false for text that simply spills past its box, so it
        // catches eliding and not overflowing. Dropping the wrap and keeping
        // ElideNone would look fine to the assertion above and print the label
        // over its neighbour.
        QVERIFY2(entry->property("contentWidth").toReal() <= entry->width() + 0.5,
                 qPrintable(QStringLiteral("`%1` is wider than its column").arg(text)));
    }
    key(Qt::Key_Escape);
    settle();
}

// Closing a pane must not leave a binding running against the pane it closed.
//
// `pragma ComponentBehavior: Bound` keeps the enclosing scope alive for one
// more evaluation, so a delegate can run with its pane already null; no
// TypeError may be printed.
void TestStudio::survivesClosingAPaneWithoutComplaint()
{
    QStringList complaints;
    const auto previous = qInstallMessageHandler(nullptr);
    static QStringList *sink = nullptr;
    sink = &complaints;
    qInstallMessageHandler([](QtMsgType type, const QMessageLogContext &, const QString &text) {
        if (sink && (type == QtWarningMsg || type == QtCriticalMsg))
            sink->append(text);
    });

    QMetaObject::invokeMethod(m_workspace, "splitRight",
                              Q_ARG(QString, QStringLiteral("files")));
    settle();
    QCOMPARE(m_workspace->paneCount(), 2);
    m_workspace->closePane(1);
    settle();
    QCOMPARE(m_workspace->paneCount(), 1);

    // The binding does not re-evaluate until the scene is rendered again, and
    // `settle` only spins the event loop, so the render is forced while still
    // listening.
    window()->grabWindow();
    QTest::qWait(60);
    settle();
    window()->grabWindow();
    settle();

    sink = nullptr;
    qInstallMessageHandler(previous);

    const QStringList errors = complaints.filter(QStringLiteral("TypeError"));
    QVERIFY2(errors.isEmpty(), qPrintable(errors.join(QLatin1Char('\n'))));
}

// A tag added by the CLI shows up without anyone pressing reload. One CLI write
// is four filesystem events, coalesced into one re-read that must not swallow
// the last of them.
void TestStudio::noticesTheIndexChangingUnderIt()
{
    const QString index = QDir(m_home.path()).filePath(QStringLiteral("omafiles/index.jsonl"));
    const int before = m_index->tagRowCount();
    QVERIFY(before > 0);

    // Appended in place, not committed by rename the way the CLI does. That is
    // deliberate and it is enough: the watch is on the file, and either shape
    // of write moves it. The rename path is covered by the CLI suite. The file
    // the fixture leaves untagged, because a second line for one path is a
    // malformed index.
    {
        QFile handle(index);
        QVERIFY(handle.open(QIODevice::Append | QIODevice::Text));
        const QString path = QDir(m_tree.path()).filePath(QStringLiteral("plain.txt"));
        handle.write(QStringLiteral("{\"path\":\"%1\",\"tags\":[\"watched\"]}\n")
                         .arg(path).toUtf8());
    }

    QTRY_VERIFY(indexModelHasTag(m_index, QStringLiteral("watched")));
    QVERIFY(m_index->tagRowCount() > before);

    // And put the fixture back, so the tests after this one see what they
    // were written against.
    writeIndex();

    // One of each kind, so the panel has something in it: `v` is on the key
    // sheet, and a key the sweep presses over an empty list proves nothing.
    {
        const QDir config(QDir(m_desktop.path()).filePath(QStringLiteral("config")));
        QVERIFY(config.mkpath(QStringLiteral("omafiles")));
        QFile views(config.filePath(QStringLiteral("omafiles/views.jsonl")));
        QVERIFY(views.open(QIODevice::WriteOnly | QIODevice::Text));
        views.write(QStringLiteral("{\"name\":\"the notes\",\"tags\":[\"notes\"]}\n"
                                   "{\"name\":\"alpha\",\"dir\":\"%1\"}\n")
                        .arg(QDir(m_tree.path()).filePath(QStringLiteral("alpha")))
                        .toUtf8());
    }


    QTRY_VERIFY(!indexModelHasTag(m_index, QStringLiteral("watched")));
    QTRY_COMPARE(m_index->tagRowCount(), before);
}

// On a machine where the command line has never run there is no index
// directory, and the window still picks up the index the first write creates.
void TestStudio::watchesTheIndexOnAFreshInstall()
{
    QTemporaryDir fresh;
    QVERIFY(fresh.isValid());
    const QByteArray previous = qgetenv("XDG_DATA_HOME");
    qputenv("XDG_DATA_HOME", fresh.path().toUtf8());

    {
        IndexModel model;
    QTRY_VERIFY(!model.loading());
        const QDir data(QDir(fresh.path()).filePath(QStringLiteral("omafiles")));
        QVERIFY2(data.exists(), "the studio did not make the directory it means to watch");
        QCOMPARE(model.tagRowCount(), 0);

        QFile index(data.filePath(QStringLiteral("index.jsonl")));
        QVERIFY(index.open(QIODevice::WriteOnly | QIODevice::Text));
        index.write(QStringLiteral("{\"path\":\"%1\",\"tags\":[\"firstrun\"]}\n")
                        .arg(QDir(m_tree.path()).filePath(QStringLiteral("one.txt")))
                        .toUtf8());
        index.close();

        QTRY_VERIFY(indexModelHasTag(&model, QStringLiteral("firstrun")));
    }

    qputenv("XDG_DATA_HOME", previous);
}

void TestStudio::watchesOwnedStateWithoutWatchingUnrelatedParents_data()
{
    QTest::addColumn<bool>("dataExists");
    QTest::addColumn<bool>("configExists");
    QTest::newRow("fresh") << false << false;
    QTest::newRow("data-ready") << true << false;
    QTest::newRow("config-ready") << false << true;
    QTest::newRow("both-ready") << true << true;
}

void TestStudio::watchesOwnedStateWithoutWatchingUnrelatedParents()
{
    QFETCH(bool, dataExists);
    QFETCH(bool, configExists);
    QTemporaryDir fresh;
    QVERIFY(fresh.isValid());
    const QByteArray dataHome = qgetenv("XDG_DATA_HOME");
    const QByteArray configHome = qgetenv("XDG_CONFIG_HOME");
    const auto previousRoots = roots();
    const auto restore = qScopeGuard([&] {
        registeredWatches = nullptr;
        setRoots(previousRoots);
        qputenv("XDG_DATA_HOME", dataHome);
        qputenv("XDG_CONFIG_HOME", configHome);
    });
    const QString data = fresh.filePath("data");
    const QString config = fresh.filePath("config");
    QVERIFY(QDir().mkpath(data));
    QVERIFY(QDir().mkpath(config));
    qputenv("XDG_DATA_HOME", data.toUtf8());
    qputenv("XDG_CONFIG_HOME", config.toUtf8());
    const QString dataState = data + "/omafiles";
    const QString configState = config + "/omafiles";
    if (dataExists) QVERIFY(QDir().mkpath(dataState));
    if (configExists) QVERIFY(QDir().mkpath(configState));

    QStringList watches;
    registeredWatches = &watches;
    IndexModel model;
    registeredWatches = nullptr;
    QVERIFY(QDir(dataState).exists());
    QVERIFY(QDir(configState).exists());
    QVERIFY2(watches.contains(dataState), qPrintable(watches.join('\n')));
    QVERIFY2(watches.contains(configState), qPrintable(watches.join('\n')));
    QVERIFY2(!watches.contains(data), "Watching the data parent observes unrelated startup writes");
    QVERIFY2(!watches.contains(config), "Watching the config parent observes unrelated startup writes");
    QTRY_VERIFY(!model.loading());
}

// The keys the bare-character sweep cannot reach.
//
// It skips anything whose spelling has more than one character, which is every
// named key and every chord -- so `Home`, `End`, `PgUp`, `PgDn`, `Ctrl+D`,
// `Ctrl+U`, `F5`, `Ctrl+R`, `Menu`, `Shift+F10` and the arrows, which the key
// sheet lists and the letter sweep does not press.
void TestStudio::answersTheNamedAndChordedKeys()
{
    QObject *root = m_engine->rootObjects().first();
    PaneState *pane = m_workspace->paneAtIndex(0);
    const QString sweep = m_tree.filePath(QStringLiteral("sweep"));

    const auto reset = [&](int row) {
        while (pane->tabCount() > 1)
            m_workspace->closeTab(0, pane->tabCount() - 1);
        pane->setCurrentIndex(0);
        tab()->setCwd(sweep);
        tab()->setViewMode(QStringLiteral("list"));
        QMetaObject::invokeMethod(root, "takeFocus");
        settle();
        QMetaObject::invokeMethod(filesRoot(), "setCursor", Q_ARG(QVariant, QVariant(row)));
        settle();
    };

    // Movement.
    reset(1);
    key(Qt::Key_Down);
    QCOMPARE(cursor(), 2);
    key(Qt::Key_Up);
    QCOMPARE(cursor(), 1);
    key(Qt::Key_End);
    QCOMPARE(cursor(), filesRoot()->property("folder").value<QObject *>()
                           ->property("count").toInt() - 1);
    key(Qt::Key_Home);
    QCOMPARE(cursor(), 0);

    reset(0);
    key(Qt::Key_PageDown);
    QVERIFY2(cursor() > 0, "PgDn did not move the cursor");
    key(Qt::Key_PageUp);
    QCOMPARE(cursor(), 0);

    reset(0);
    key(Qt::Key_D, Qt::ControlModifier);
    const int half = cursor();
    QVERIFY2(half > 0, "Ctrl+D did not move the cursor");
    key(Qt::Key_U, Qt::ControlModifier);
    QVERIFY2(cursor() < half, "Ctrl+U did not move the cursor back");

    // Walking the tree with the named keys.
    reset(1);
    key(Qt::Key_Right);
    QVERIFY2(cwd() != sweep, "Right did not enter the folder under the cursor");
    key(Qt::Key_Left);
    QCOMPARE(cwd(), sweep);
    reset(1);
    key(Qt::Key_Backspace);
    QVERIFY2(cwd() != sweep, "Backspace did not go up");

    // The menu, by both of its named spellings.
    for (const auto &press : {QPair<Qt::Key, Qt::KeyboardModifiers>(Qt::Key_Menu,
                                                                    Qt::NoModifier),
                              QPair<Qt::Key, Qt::KeyboardModifiers>(Qt::Key_F10,
                                                                    Qt::ShiftModifier)}) {
        reset(1);
        key(press.first, press.second);
        QVERIFY2(root->property("blocked").toBool(),
                 "a named menu key did not open the menu");
        key(Qt::Key_Escape);
        settle();
    }

    // Reload, by both spellings.
    //
    // Not observed through the index's contents: the window watches the file
    // and fetches a change on its own, so a row written behind its back is
    // already there by the time the key is pressed. What the key has to do is
    // ask -- so the test presses it with nothing changed and requires the ask.
    for (int i = 0; i < 2; ++i) {
        reset(1);
        QSignalSpy asked(m_index, SIGNAL(changed()));
        if (i == 0)
            key(Qt::Key_F5);
        else
            key(Qt::Key_R, Qt::ControlModifier);
        settle();
        QVERIFY2(asked.count() >= 1,
                 i == 0 ? "F5 did not reload the index"
                        : "Ctrl+R did not reload the index");
    }

    // Panes, by the arrow spelling the sweep never reaches.
    reset(1);
    key(Qt::Key_Backslash, Qt::ControlModifier);
    QCOMPARE(m_workspace->paneCount(), 2);
    QCOMPARE(m_workspace->focusedPane(), 1);
    key(Qt::Key_Left, Qt::AltModifier);
    QCOMPARE(m_workspace->focusedPane(), 0);
    key(Qt::Key_Right, Qt::AltModifier);
    QCOMPARE(m_workspace->focusedPane(), 1);
    key(Qt::Key_W, Qt::ControlModifier | Qt::ShiftModifier);
    QCOMPARE(m_workspace->paneCount(), 1);

    // The arrows in the icons view, where they move along the row rather than
    // walk the tree as they do in the list.
    reset(1);
    tab()->setViewMode(QStringLiteral("icons"));
    settle();
    {
        const int before = cursor();
        key(Qt::Key_Right);
        QVERIFY2(cursor() > before, "Right did not move to the next icon");
        key(Qt::Key_Left);
        QCOMPARE(cursor(), before);
        QVERIFY2(cwd() == sweep, "an arrow in the icons view walked the tree");
    }
    tab()->setViewMode(QStringLiteral("list"));
    settle();

    // The prompts, by their named spellings; the bare keys are swept.
    reset(1);
    key(Qt::Key_F2);
    QCOMPARE(root->property("prompt").toString(), QStringLiteral("rename"));
    key(Qt::Key_Escape);
    reset(1);
    key(Qt::Key_N, Qt::ControlModifier | Qt::ShiftModifier);
    QCOMPARE(root->property("prompt").toString(), QStringLiteral("folder"));
    key(Qt::Key_Escape);

    // Enter, which the sheet spells as a word.
    reset(1);
    key(Qt::Key_Return);
    QVERIFY2(cwd() != sweep, "Enter did not enter the folder under the cursor");

    reset(0);
    tab()->setCwd(m_tree.path());
    settle();
}

// The sheet under `?` is the window's own claim about what it answers. Every
// single-character key on it is pressed here, and the window has to be alive
// after each one -- a documented key that hangs the keyboard is worse than an
// undocumented one.
void TestStudio::everyDocumentedKeyIsAnswered()
{
    QObject *root = m_engine->rootObjects().first();
    PaneState *pane = m_workspace->paneAtIndex(0);

    // What each bare key needs before it can show that it did anything. Several
    // are honest no-ops from a standing start -- `A` with nothing marked, `1`
    // already in the list view, `]` with one tab, and `g`, which is half of
    // `gg` -- and a sweep that pressed them cold would be asserting that
    // nothing happened.
    struct Case { const char *key; const char *setup; };
    static const Case cases[] = {
        {"j", ""},        {"k", ""},        {"l", ""},        {"h", ""},
        {"~", ""},        {"t", ""},        {"v", ""},        {"d", ""},        {"m", ""},
        {"s", ""},        {"i", ""},
        {"S", ""},        {"G", ""},        {"a", ""},        {".", ""},
        {"g", "twice"},                    // one `g` only arms the second
        {"A", "mark"},                     // needs something to clear
        {"1", "icons"},   {"2", "list"},    {"3", "list"},
        {"]", "tabs"},    {"[", "tabs"},
        {"/", ""},                         // opens the filter bar
        {"r", ""},        {"n", ""},       // open a prompt in it
        {"*", ""},        {"f", ""},        {"F", ""},
        {"J", ""},        {"K", ""},
    };

    // The other half of the sheet: the rows spelled as words. The sweep below
    // cannot press those -- `Ctrl d` is one chord written as two words, `Bksp`
    // is a name -- so they are driven by `answersTheNamedAndChordedKeys` and by
    // the region tests, and what this list does is make the author say where.
    //
    // Weaker than the sweep, and worth having: without it a row spelled in more
    // than one character goes onto the sheet with no test asking about it, which
    // is how `Ctrl Tab` came to claim two moves for one chord.
    static const char *named[] = {
        "↓ ↑", "Ctrl d", "Ctrl u", "PgDn PgUp", "Home End", "← →",
        "Enter", "→", "←", "Bksp", "Ctrl l", "F5", "Ctrl r",
        "Space", "Menu", "Shift F10",
        // opensSidebarMenusByTouchAndKeyboard
        "m Menu Shift F10",
        "Ctrl h",
        "Esc", "← Esc",
        // worksTabsAndPanesFromTheKeyboard
        "Ctrl t", "Ctrl w", "Delete", "Ctrl Tab", "Ctrl Shift Tab", "Alt 1…9",
        "Ctrl \\", "Ctrl Shift w", "Alt h l", "Alt ← →",
        // resizesTheSplitWithCtrlComma
        "Ctrl ,", "Ctrl .",
        "Ctrl p", "Tab",
        // navigatesArchiveControls
        "Tab Shift+Tab", "Escape",
        "F2", "Ctrl Shift n",
        // copiesCutsAndPastesThroughTheDesktopClipboard
        "Ctrl c", "Ctrl x", "Ctrl v",
        // linksClipboardFilesByKeyboard
        "Ctrl Shift v",
        // offersUndoByKeyboardPaletteAndPointer
        "Ctrl z",
    };

    // The keymap drives the list, so a key added to the sheet without a case
    // here fails rather than going unchecked.
    QSet<QString> covered;
    for (const Case &one : cases)
        covered.insert(QString::fromLatin1(one.key));
    QSet<QString> driven;
    for (const char *one : named)
        driven.insert(QString::fromUtf8(one));

    const QVariantList groups = root->property("keymap").toList();
    QVERIFY2(!groups.isEmpty(), "the window has no keymap to check against");
    for (const QVariant &group : groups) {
        const QMap<QString, QVariant> section = group.toMap();
        // Graph, preview and Trash keys are driven by their context-specific tests.
        if (section.value(QStringLiteral("title")).toString()
                .contains(QLatin1String("graph")) || section.value(QStringLiteral("title")) == "preview"
                || section.value(QStringLiteral("title")) == "trash"
                || section.value(QStringLiteral("title")) == "devices")
            continue;

        for (const QVariant &entry : section.value(QStringLiteral("keys")).toList()) {
            const QStringList parts = entry.toMap().value(QStringLiteral("key"))
                                          .toString().split(QLatin1Char(' '));
            // One spelling, several keys: `j k` is two bare keys, `Ctrl d` is
            // one chord. A part longer than a character names a modifier or a
            // named key; those are driven by answersTheNamedAndChordedKeys.
            bool bare = true;
            for (const QString &part : parts) {
                if (part.size() != 1 || part.at(0).unicode() >= 128
                    || !part.at(0).isPrint())
                    bare = false;
            }
            if (!bare) {
                const QString spelling = entry.toMap().value(QStringLiteral("key")).toString();
                QVERIFY2(driven.contains(spelling),
                         qPrintable(QStringLiteral("`%1` is on the key sheet and no test "
                                                   "says which case drives it")
                                        .arg(spelling)));
                continue;
            }

            for (const QString &part : parts) {
                // `:` and `?` open sheets that take the keyboard; drawsItself
                // drives those.
                if (part == QLatin1String(":") || part == QLatin1String("?"))
                    continue;
                QVERIFY2(covered.contains(part),
                         qPrintable(QStringLiteral("`%1` is on the key sheet and this "
                                                   "test does not say what it should do")
                                        .arg(part)));
            }
        }
    }

    const QString sweep = m_tree.filePath(QStringLiteral("sweep"));

    for (const Case &one : cases) {
        const QString setup = QString::fromLatin1(one.setup);

        // A known place with somewhere to go in every direction.
        while (pane->tabCount() > 1)
            m_workspace->closeTab(0, pane->tabCount() - 1);
        pane->setCurrentIndex(0);
        tab()->setCwd(sweep);
        tab()->setViewMode(setup == QLatin1String("icons") ? QStringLiteral("icons")
                                                            : QStringLiteral("list"));
        tab()->setSortRole(QStringLiteral("name"));
        tab()->setSortAscending(true);
        tab()->setSelectedTag(QString());
        tab()->setShowHidden(false);
        QMetaObject::invokeMethod(root, "takeFocus");
        settle();

        QObject *folder = filesRoot()->property("folder").value<QObject *>();
        QMetaObject::invokeMethod(folder, "clearMarks");
        if (setup == QLatin1String("mark"))
            QMetaObject::invokeMethod(folder, "toggleMarkAt", Q_ARG(int, 2));
        if (setup == QLatin1String("tabs"))
            m_workspace->addTab(QStringLiteral("files"));
        QMetaObject::invokeMethod(filesRoot(), "setCursor", Q_ARG(QVariant, QVariant(1)));
        settle();

        const QString cwdBefore = cwd();
        const QString viewBefore = tab()->viewMode();
        const bool hiddenBefore = tab()->showHidden();
        const QString sortBefore = tab()->sortRole();
        const bool ascBefore = tab()->sortAscending();
        const QString regionBefore = root->property("focusRegion").toString();
        const bool blockedBefore = root->property("blocked").toBool();
        const int cursorBefore = cursor();
        const int markedBefore = folder->property("markedCount").toInt();
        const int tabBefore = pane->currentIndex();

        key(one.key[0]);
        if (setup == QLatin1String("twice"))
            key(one.key[0]);

        QObject *now = filesRoot() ? filesRoot()->property("folder").value<QObject *>()
                                    : nullptr;
        const bool answered = cursor() != cursorBefore
            || cwd() != cwdBefore
            || tab()->viewMode() != viewBefore
            || tab()->showHidden() != hiddenBefore
            || tab()->sortRole() != sortBefore
            || tab()->sortAscending() != ascBefore
            || root->property("focusRegion").toString() != regionBefore
            || root->property("blocked").toBool() != blockedBefore
            || pane->currentIndex() != tabBefore
            || (now && now->property("markedCount").toInt() != markedBefore);

        QVERIFY2(answered,
                 qPrintable(QStringLiteral("`%1` is on the key sheet and changed nothing "
                                           "the window can be asked about")
                                .arg(QChar::fromLatin1(one.key[0]))));

        key(Qt::Key_Escape);
        settle();
    }

    while (pane->tabCount() > 1)
        m_workspace->closeTab(0, pane->tabCount() - 1);
    tab()->setCwd(m_tree.path());
    tab()->setShowHidden(false);
    QMetaObject::invokeMethod(root, "takeFocus");
    settle();
}

void TestStudio::drawsItself()
{
    focusRow(QStringLiteral("two.md"));
    shot(QStringLiteral("list"));

    key('2');
    shot(QStringLiteral("details-view"));

    key('3');
    shot(QStringLiteral("icons"));

    key('1');
    key('t');
    // The handoff, asserted rather than assumed: `focusTags` returns silently
    // when the panel is not there yet, and the Enters below are for the panel.
    QCOMPARE(m_engine->rootObjects().first()->property("focusRegion").toString(),
             QStringLiteral("tags"));
    key(Qt::Key_Return);
    shot(QStringLiteral("tag-filter"));
    key(Qt::Key_Return);
    key(Qt::Key_Escape);

    key('/');
    shot(QStringLiteral("filtering"));
    key(Qt::Key_Escape);

    key('?');
    shot(QStringLiteral("03-keys"));
    key(Qt::Key_Escape);

    key(':');
    shot(QStringLiteral("commands"));
    key(Qt::Key_Escape);

    m_workspace->addTab(QStringLiteral("graph"));
    settle();
    shot(QStringLiteral("04-graph"));
    m_workspace->closeCurrentTab();
    settle();

    QMetaObject::invokeMethod(m_workspace, "splitRight", Q_ARG(QString, QStringLiteral("files")));
    settle();
    shot(QStringLiteral("split"));
    QMetaObject::invokeMethod(m_workspace, "closePane", Q_ARG(int, 1));
    settle();

    QVERIFY2(window()->activeFocusItem() != nullptr,
             "the keyboard was lost somewhere in the tour");
}

// The pictures in `docs/`, taken from a folder built for the purpose.
//
// The fixture the rest of this file uses is named for what each test needs to
// say -- `alpha`, `one.txt`, `plain.txt` -- which is right for a test and wrong
// for a picture: a screenshot of `two.md` in `/tmp/tst_studio-AwEofN` teaches
// nobody what the program is for. This one is a folder somebody might have,
// with sizes and dates far enough apart that the columns have something to
// show.
//
// Skipped unless asked for. Docs are rebuilt on purpose, not as a side effect
// of running the tests, and this one rewrites the index the other tests read --
// which is why it runs last.
void TestStudio::writesTheDocumentationShots()
{
    const QString into = QString::fromLocal8Bit(qgetenv("OMAFILES_SHOTS"));
    if (qgetenv("OMAFILES_DOCS").isEmpty() || into.isEmpty())
        QSKIP("set OMAFILES_DOCS=1 and OMAFILES_SHOTS=<dir> to write the docs images");

    // Fixed dates and a fixed path, so regenerating the images twice gives the
    // same bytes twice: the path is in the picture, on the address bar, so a
    // unique directory per run would put a different string in every image.
    //
    // `/tmp` spelled out rather than `QDir::tempPath()`, which honours `TMPDIR`
    // and would put a different string on the address bar.
    //
    // Fixed and shared, so the name is held under a lock for the length of the
    // run and a second run waits rather than writing the same fixture.
    const QString fixture = QStringLiteral("/tmp/omafiles-docs");
    QVERIFY2(QDir().mkpath(fixture),
             "the documentation pictures need a writable /tmp; the path is in the picture");
    QLockFile held(fixture + QStringLiteral(".lock"));
    // Long enough for a second checkout on the same machine to finish rather
    // than for this one to fail: the fixture path has to be fixed, because it
    // shows in the address bar of every picture, so two runs cannot each have
    // their own and the only options are to queue or to give up.
    QVERIFY2(held.tryLock(120000),
             "another run has held the documentation fixture for two minutes");

    const QString root = fixture + QStringLiteral("/workshop");
    QDir(fixture).removeRecursively();
    QDir().mkpath(root + QStringLiteral("/contracts/2026"));
    QDir().mkpath(root + QStringLiteral("/invoices"));
    QDir().mkpath(root + QStringLiteral("/scans"));

    // A folder made a moment ago is dated a moment ago, so without this the
    // three folder rows carry the clock and the picture differs on every run.
    const auto dateIt = [](const QString &path, const char *when) {
        const QDateTime at = QDateTime::fromString(QString::fromLatin1(when),
                                                   QStringLiteral("yyyy-MM-dd HH:mm"));
        QVERIFY(at.isValid());
        const timespec times[2] = {
            {at.toSecsSinceEpoch(), 0},          // accessed
            {at.toSecsSinceEpoch(), 0},          // modified
        };
        QCOMPARE(::utimensat(AT_FDCWD, QFile::encodeName(path).constData(), times, 0), 0);
    };
    dateIt(root + QStringLiteral("/contracts"), "2026-08-30 20:17");
    dateIt(root + QStringLiteral("/invoices"), "2026-07-21 15:48");
    dateIt(root + QStringLiteral("/scans"), "2026-08-28 07:44");

    struct Sample { const char *file; int kb; const char *when; const char *tags; };
    const Sample samples[] = {
        {"archive-2026.zip",     26, "2026-08-26 20:19", "\"archive\",\"backup\""},
        {"budget.ods",           13, "2026-02-02 15:33", "\"finance\",\"spreadsheet\""},
        {"invoice-4471.pdf",      8, "2026-08-07 12:42", "\"finance\",\"invoice\""},
        {"keyring-fix.md",        7, "2026-08-24 17:29", "\"linux\",\"notes\""},
        {"network.drawio",       21, "2026-02-09 18:22", "\"diagram\",\"home\",\"network\""},
        {"q3-report.ods",        14, "2026-02-19 16:01", "\"finance\",\"q3\",\"spreadsheet\""},
        {"restore-nas.md",       24, "2026-08-14 18:01", "\"backup\",\"notes\""},
        {"scratch.txt",           1, "2026-08-30 09:04", ""},
    };

    QFile index(QDir(m_home.path()).filePath(QStringLiteral("omafiles/index.jsonl")));
    QVERIFY(index.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text));

    for (const Sample &sample : samples) {
        const QString path = QDir(root).filePath(QString::fromLatin1(sample.file));
        {
            QFile file(path);
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write(QByteArray(sample.kb * 1024, 'x'));
        }

        // Dated after the write, and on a second handle: the flush at close
        // would put the clock back to now and the modified column would be one
        // date repeated eight times.
        const QDateTime when = QDateTime::fromString(QString::fromLatin1(sample.when),
                                                     QStringLiteral("yyyy-MM-dd HH:mm"));
        QVERIFY(when.isValid());
        {
            QFile file(path);
            QVERIFY(file.open(QIODevice::ReadWrite));
            QVERIFY(file.setFileTime(when, QFileDevice::FileModificationTime));
        }

        if (*sample.tags == '\0')
            continue;                            // one row with no tags, on purpose
        index.write(QStringLiteral("{\"path\":\"%1\",\"tags\":[%2]}\n")
                        .arg(path, QString::fromLatin1(sample.tags))
                        .toUtf8());
    }
    index.close();

    // Views of its own, about the workshop in the picture. The fixture the rest
    // of this file uses has two that mean nothing here -- a folder that is not
    // in this tree, and a tag nothing in it carries -- and a documentation
    // picture is the one place where what is on screen has to be about what is
    // on screen.
    {
        QFile views(QDir(QDir(m_desktop.path()).filePath(QStringLiteral("config")))
                        .filePath(QStringLiteral("omafiles/views.jsonl")));
        QVERIFY(views.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text));
        views.write(QStringLiteral("{\"name\":\"to file\",\"dir\":\"%1/scans\"}\n"
                                   "{\"name\":\"this quarter\",\"tags\":[\"q3\"]}\n"
                                   "{\"name\":\"receipts\",\"tags\":[\"finance\",\"invoice\"]}\n")
                        .arg(root)
                        .toUtf8());
    }
    m_views->reload();

    m_index->reload();
    settle();
    tab()->setCwd(root);
    tab()->setSortRole(QStringLiteral("name"));
    tab()->setSortAscending(true);
    settle();
    focusRow(QStringLiteral("network.drawio"));

    shot(QStringLiteral("01-studio"), Shot::Documentation);

    key('2');
    shot(QStringLiteral("02-details"), Shot::Documentation);
    key('1');

    key('?');
    shot(QStringLiteral("03-keys"), Shot::Documentation);
    key(Qt::Key_Escape);

    m_workspace->addTab(QStringLiteral("graph"));
    settle();
    shot(QStringLiteral("04-graph"), Shot::Documentation);
    m_workspace->closeCurrentTab();
    settle();
}

// A colour scheme is replaced, not edited: a theme switcher and an editor alike
// write a temporary and rename it over the file. The watch goes with the file it
// was taken on, so a window that does not re-take it follows the first change
// and no later one -- and the desktop and the window disagree about the theme
// until a restart.
void TestStudio::followsEveryThemeChangeAndNotJustTheFirst()
{
    QTemporaryDir state;
    QVERIFY(state.isValid());
    const QString themeDir =
        QDir(state.path()).filePath(QStringLiteral("omarchy/current/theme"));
    QVERIFY(QDir().mkpath(themeDir));
    const QString colours = QDir(themeDir).filePath(QStringLiteral("colors.toml"));

    const auto paint = [&colours](const QString &background) {
        const QString scratch = colours + QStringLiteral(".new");
        {
            QFile file(scratch);
            if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text))
                return false;
            const QByteArray body =
                QStringLiteral("background = \"%1\"\n").arg(background).toUtf8();
            if (file.write(body) != body.size())
                return false;
        }
        // `rename(2)` and not QFile::rename, which refuses a destination that is
        // already there -- and a destination that is already there is the whole
        // of what this is about.
        return ::rename(QFile::encodeName(scratch).constData(),
                        QFile::encodeName(colours).constData()) == 0;
    };

    QVERIFY(paint(QStringLiteral("#101010")));

    const QByteArray previous = qgetenv("XDG_STATE_HOME");
    qputenv("XDG_STATE_HOME", state.path().toUtf8());
    const auto restore = qScopeGuard([&previous] { qputenv("XDG_STATE_HOME", previous); });

    Theme theme;
    QCOMPARE(theme.background(), QColor(QStringLiteral("#101010")));

    QVERIFY(paint(QStringLiteral("#202020")));
    QTRY_COMPARE(theme.background(), QColor(QStringLiteral("#202020")));

    // The one that fails without the re-arm: by now the file the watch was taken
    // on has been unlinked once already.
    QVERIFY(paint(QStringLiteral("#303030")));
    QTRY_COMPARE(theme.background(), QColor(QStringLiteral("#303030")));

    QVERIFY(paint(QStringLiteral("#404040")));
    QTRY_COMPARE(theme.background(), QColor(QStringLiteral("#404040")));
}

// Two tabs in one pane are drawn by one model. Marks kept on the model would
// belong to the pane: mark three rows, switch tab, and they are still counted,
// still shown and still what a destructive key would act on -- against a folder
// you cannot see. They belong to the tab, the way the filter and the view mode
// do.
void TestStudio::keepsMarksInTheTabTheyWereMadeIn()
{
    PaneState *pane = m_workspace->paneAtIndex(0);
    QVERIFY(pane);
    const auto folder = [this] {
        return filesRoot()->property("folder").value<QObject *>();
    };
    const auto markedRow = [this](const QString &name) {
        focusRow(name);
        return filesRoot()->property("entry").toMap()
            .value(QStringLiteral("marked")).toBool();
    };

    focusRow(QStringLiteral("one.txt"));
    key(Qt::Key_Space);
    focusRow(QStringLiteral("two.md"));
    key(Qt::Key_Space);
    QCOMPARE(folder()->property("markedCount").toInt(), 2);

    // A second tab in the same pane, standing in the same folder -- so nothing
    // about the directory changes and only the tab does.
    m_workspace->addTab(QStringLiteral("files"));
    settle();
    QCOMPARE(pane->tabCount(), 2);
    QCOMPARE(cwd(), m_tree.path());
    QVERIFY2(folder()->property("markedCount").toInt() == 0,
             "a new tab opened holding the marks made in the tab beside it");

    focusRow(QStringLiteral("three.ods"));
    key(Qt::Key_Space);
    QCOMPARE(folder()->property("markedCount").toInt(), 1);
    QVERIFY(markedRow(QStringLiteral("three.ods")));
    QVERIFY(!markedRow(QStringLiteral("one.txt")));

    // And back, to the two rows this tab marked and no others.
    pane->setCurrentIndex(0);
    settle();
    QCOMPARE(folder()->property("markedCount").toInt(), 2);
    QVERIFY(markedRow(QStringLiteral("one.txt")));
    QVERIFY(markedRow(QStringLiteral("two.md")));
    QVERIFY(!markedRow(QStringLiteral("three.ods")));

    // Walking this tab into another folder ends them: they name files that are
    // no longer on screen.
    tab()->setCwd(m_tree.filePath(QStringLiteral("beta")));
    settle();
    QCOMPARE(folder()->property("markedCount").toInt(), 0);

    while (pane->tabCount() > 1)
        m_workspace->closeTab(0, pane->tabCount() - 1);
    tab()->setCwd(m_tree.path());
    settle();
}

// The list is the folder, so it follows the folder. Anything else writing here
// -- another terminal, a download, `omafiles` itself in the next window -- shows
// up without a key being pressed, and `F5` goes on meaning what the key sheet
// says it means.
void TestStudio::followsTheFolderOnDisk()
{
    QTemporaryDir scratch;
    QVERIFY(scratch.isValid());
    const QDir here(scratch.path());
    const auto write = [&here](const QString &name) {
        QFile file(here.filePath(name));
        return file.open(QIODevice::WriteOnly) && file.write("x") == 1;
    };
    QVERIFY(write(QStringLiteral("first.txt")));

    tab()->setCwd(scratch.path());
    settle();
    QObject *folder = filesRoot()->property("folder").value<QObject *>();
    QCOMPARE(folder->property("count").toInt(), 2);          // `..` and one file

    QVERIFY(write(QStringLiteral("second.txt")));
    QTRY_COMPARE(folder->property("count").toInt(), 3);
    QVERIFY2(rowOf(QStringLiteral("second.txt")) >= 0,
             "the folder grew a file and the list did not");

    QVERIFY(QFile::remove(here.filePath(QStringLiteral("first.txt"))));
    QTRY_COMPARE(folder->property("count").toInt(), 2);
    QCOMPARE(rowOf(QStringLiteral("first.txt")), -1);

    // A folder is watched wherever the tab walks, not only the one it started
    // in, so the watch has to be re-taken on every read.
    const QString deeper = here.filePath(QStringLiteral("deeper"));
    QVERIFY(QDir().mkpath(deeper));
    tab()->setCwd(deeper);
    settle();
    QCOMPARE(filesRoot()->property("folder").value<QObject *>()
                 ->property("count").toInt(), 1);            // `..` and nothing else
    {
        QFile file(QDir(deeper).filePath(QStringLiteral("later.txt")));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("x");
    }
    QTRY_COMPARE(filesRoot()->property("folder").value<QObject *>()
                     ->property("count").toInt(), 2);

    tab()->setCwd(m_tree.path());
    settle();
}

// The number beside a folder is every name it holds, dangling links, links
// round to themselves and fifos included.
void TestStudio::countsEveryNameAFolderHolds()
{
    QTemporaryDir scratch;
    QVERIFY(scratch.isValid());
    const QString odd = QDir(scratch.path()).filePath(QStringLiteral("odd"));
    QVERIFY(QDir().mkpath(odd));
    const QDir inside(odd);
    // A folder nobody may open, made alongside the other one so both are there
    // before the list is drawn.
    const QString shut = QDir(scratch.path()).filePath(QStringLiteral("shut"));
    QVERIFY(QDir().mkpath(shut));
    QVERIFY(QFile::setPermissions(shut, QFileDevice::Permissions()));
    const auto reopen = qScopeGuard([&shut] {
        QFile::setPermissions(shut, QFileDevice::ReadOwner | QFileDevice::WriteOwner
                                        | QFileDevice::ExeOwner);
    });

    QVERIFY(QFile::link(QStringLiteral("/nowhere/at/all"),
                        inside.filePath(QStringLiteral("dangling"))));
    QVERIFY(QFile::link(QStringLiteral("loop"), inside.filePath(QStringLiteral("loop"))));
    QCOMPARE(::mkfifo(QFile::encodeName(inside.filePath(QStringLiteral("pipe"))).constData(),
                      0644), 0);
    {
        // A dotfile counts: the number is what the folder holds, not what one
        // view of it would draw.
        QFile file(inside.filePath(QStringLiteral(".hidden")));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("x");
    }
    // And the name that is bytes rather than text. It cannot be listed as a row,
    // and it is still one of the things in there.
    const QByteArray unnameable = QFile::encodeName(odd) + "/bad\xffname";
    const int fd = ::open(unnameable.constData(), O_CREAT | O_WRONLY, 0644);
    QVERIFY(fd >= 0);
    ::close(fd);
    const auto sweep = qScopeGuard([&unnameable] { ::unlink(unnameable.constData()); });

    tab()->setCwd(scratch.path());
    settle();
    focusRow(QStringLiteral("odd"));
    QCOMPARE(filesRoot()->property("entry").toMap()
                 .value(QStringLiteral("items")).toInt(), 5);

    // A folder nobody may open is not a folder with nothing in it, and the panel
    // says so by not putting a number there at all.
    focusRow(QStringLiteral("shut"));
    QCOMPARE(filesRoot()->property("entry").toMap()
                 .value(QStringLiteral("items")).toInt(), -1);

    tab()->setCwd(m_tree.path());
    settle();
}


void TestStudio::pickSortRoleTogglesOnSameRole()
{
    tab()->pickSortRole(QStringLiteral("name"));
    const bool before = tab()->sortAscending();
    tab()->pickSortRole(QStringLiteral("name"));
    QVERIFY(before != tab()->sortAscending());
}


// Dolphin and Nautilus both end a clipped tag row with +N rather than silently
// dropping names. The strip exposes how many are hidden so a test can prove the
// count matches what opacity hid, with space reserved for the chip itself.
void TestStudio::tagStripSaysHowManyTagsDoNotFit()
{
    const QString path = QDir(m_tree.path()).filePath(QStringLiteral("manytags.txt"));
    {
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("x");
    }

    QStringList tags;
    for (int i = 0; i < 16; ++i)
        tags << QString(QChar('a' + i));

    putIndexRow(path, tags);
    m_index->reload();
    settle();
    settle();

    tab()->setViewMode(QStringLiteral("details"));
    settle();
    focusRow(QStringLiteral("manytags.txt"));
    settle();

    QObject *overflowing = nullptr;
    for (QQuickItem *item : itemsNamed(QStringLiteral("tagStrip"))) {
        if (item->property("tags").toStringList().size() != tags.size())
            continue;
        if (!item->isVisible())
            continue;
        if (item->property("hiddenCount").toInt() <= 0)
            continue;
        overflowing = item;
        break;
    }
    QVERIFY2(overflowing, "no tag strip reported hidden tags for manytags.txt");
    QCOMPARE(overflowing->property("tags").toStringList(), tags);
    QVERIFY(overflowing->property("hiddenCount").toInt() > 0);
    QCOMPARE(overflowing->property("hiddenCount").toInt()
                 + overflowing->property("visibleCount").toInt(),
             tags.size());

    auto *strip = qobject_cast<QQuickItem *>(overflowing);
    QVERIFY(strip);
    const bool alignRight = strip->property("alignRight").toBool();
    const QString remainderName = alignRight
            ? QStringLiteral("tagStripOverflowLeading")
            : QStringLiteral("tagStripOverflowTrailing");
    QQuickItem *remainder = nullptr;
    QList<QQuickItem *> queue{strip};
    while (!queue.isEmpty()) {
        QQuickItem *item = queue.takeFirst();
        if (item->objectName() == remainderName) {
            remainder = item;
            break;
        }
        queue.append(item->childItems());
    }
    QVERIFY2(remainder, "no +N remainder chip on overflowing tag strip");
    QVERIFY(remainder->isVisible());
    const QPointF topLeft = remainder->mapToItem(strip, QPointF(0, 0));
    QVERIFY2(topLeft.x() >= 0.0,
             qPrintable(QStringLiteral("+N chip x=%1 is left of strip").arg(topLeft.x())));
    QVERIFY2(topLeft.x() + remainder->width() <= strip->width() + 0.5,
             qPrintable(QStringLiteral("+N chip right edge %1 exceeds strip width %2")
                                .arg(topLeft.x() + remainder->width())
                                .arg(strip->width())));

    focusRow(QStringLiteral("two.md"));
    settle();

    QObject *fits = nullptr;
    for (QQuickItem *item : itemsNamed(QStringLiteral("tagStrip"))) {
        if (item->property("tags").toStringList().size() != 1)
            continue;
        fits = item;
        break;
    }
    QVERIFY2(fits, "no tag strip found for two.md");
    QCOMPARE(fits->property("hiddenCount").toInt(), 0);
    QCOMPARE(fits->property("visibleCount").toInt(), 1);
}

void TestStudio::namesTheMenuRowTheOverflowAndTheTabClose()
{
    focusRow(QStringLiteral("two.md"));
    QObject *root = m_engine->rootObjects().first();

    key('m');
    QVERIFY2(root->property("blocked").toBool(), "m did not open the row menu");

    QQuickItem *menuItem = findAccessibleItem(window()->contentItem(),
                                              QAccessible::MenuItem,
                                              QStringLiteral("Open"));
    if (!menuItem) {
        QQuickItem *menuPopup = findAccessibleItem(window()->contentItem(),
                                                   QAccessible::PopupMenu,
                                                   QStringLiteral("Menu"));
        QVERIFY2(menuPopup, "no row menu popup exposed to assistive tech");
        QObject *list = nullptr;
        QList<QQuickItem *> queue{menuPopup};
        while (!queue.isEmpty() && !list) {
            QQuickItem *item = queue.takeFirst();
            if (item->metaObject()->className() == QByteArray("QQuickListView"))
                list = item;
            queue.append(item->childItems());
        }
        QVERIFY2(list, "row menu has no list for the current item");
        menuItem = viewItemAt(list, list->property("currentIndex").toInt());
    }
    QVERIFY(menuItem);
    QAccessibleInterface *menuIface = accessibleOn(menuItem);
    if (!menuIface)
        QSKIP("QAccessible cannot see QML attached Accessible names in offscreen");
    QCOMPARE(menuIface->text(QAccessible::Name), QStringLiteral("Open"));
    key(Qt::Key_Escape);
    settle();

    const QString path = QDir(m_tree.path()).filePath(QStringLiteral("manytags.txt"));
    {
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("x");
    }
    QStringList tags;
    for (int i = 0; i < 16; ++i)
        tags << QString(QChar('a' + i));
    putIndexRow(path, tags);
    m_index->reload();
    settle();
    settle();
    tab()->setViewMode(QStringLiteral("details"));
    settle();
    focusRow(QStringLiteral("manytags.txt"));
    settle();

    QObject *overflowing = nullptr;
    for (QQuickItem *item : itemsNamed(QStringLiteral("tagStrip"))) {
        if (item->property("tags").toStringList().size() != tags.size())
            continue;
        if (!item->isVisible())
            continue;
        if (item->property("hiddenCount").toInt() <= 0)
            continue;
        overflowing = item;
        break;
    }
    QVERIFY2(overflowing, "no tag strip reported hidden tags for manytags.txt");

    auto *strip = qobject_cast<QQuickItem *>(overflowing);
    QVERIFY(strip);
    const bool alignRight = strip->property("alignRight").toBool();
    const QString remainderName = alignRight
            ? QStringLiteral("tagStripOverflowLeading")
            : QStringLiteral("tagStripOverflowTrailing");
    QQuickItem *remainder = nullptr;
    QList<QQuickItem *> queue{strip};
    while (!queue.isEmpty()) {
        QQuickItem *item = queue.takeFirst();
        if (item->objectName() == remainderName) {
            remainder = item;
            break;
        }
        queue.append(item->childItems());
    }
    QVERIFY2(remainder, "no +N remainder chip on overflowing tag strip");
    QAccessibleInterface *overflowIface = accessibleOn(remainder);
    if (!overflowIface)
        QSKIP("QAccessible cannot see QML attached Accessible names in offscreen");
    QVERIFY2(overflowIface->text(QAccessible::Name).contains(QStringLiteral("more tags")),
             qPrintable(QStringLiteral("overflow accessible name was: %1")
                                .arg(overflowIface->text(QAccessible::Name))));

    const QList<QQuickItem *> handles = itemsNamed(QStringLiteral("tabHandle"));
    QVERIFY2(!handles.isEmpty(), "no tab handle to read a close mark from");
    QAccessibleInterface *closeIface = nullptr;
    queue = QList<QQuickItem *>{handles.first()};
    while (!queue.isEmpty() && !closeIface) {
        QQuickItem *item = queue.takeFirst();
        if (QAccessibleInterface *iface = accessibleOn(item)) {
            if (iface->role() == QAccessible::Button) {
                closeIface = iface;
                break;
            }
        }
        queue.append(item->childItems());
    }
    if (!closeIface) {
        if (QAccessibleInterface *tabIface = accessibleOn(handles.first())) {
            for (int i = 0; i < tabIface->childCount(); ++i) {
                if (QAccessibleInterface *child = tabIface->child(i)) {
                    if (child->role() == QAccessible::Button) {
                        closeIface = child;
                        break;
                    }
                }
            }
        }
    }
    if (!closeIface && !accessibleOn(handles.first()))
        QSKIP("QAccessible cannot see QML attached Accessible names in offscreen");
    QVERIFY2(closeIface, "no Close tab button exposed to assistive tech");
    QCOMPARE(closeIface->text(QAccessible::Name), QStringLiteral("Close tab"));
}

void TestStudio::reachesThePathBarWithTab()
{
    QObject *root = m_engine->rootObjects().first();
    QQuickItem *before = window()->activeFocusItem();

    bool reachedCrumbs = false;
    for (int i = 0; i < 24; ++i) {
        key(Qt::Key_Tab);
        if (focusIsInsidePathCrumbs(window())) {
            reachedCrumbs = true;
            break;
        }
    }
    QVERIFY2(reachedCrumbs, "Tab did not reach the path crumbs");

    key(Qt::Key_Return);
    QVERIFY2(root->property("editingPath").toBool(),
             "Return on path crumbs did not start path edit");
    key(Qt::Key_Escape);
    QVERIFY2(!root->property("editingPath").toBool(),
             "Escape did not cancel path edit");

    key(Qt::Key_Escape);
    QCOMPARE(window()->activeFocusItem(), before);
    QCOMPARE(root->property("focusRegion").toString(), QStringLiteral("list"));

    const int cursorBefore = cursor();
    key('j');
    QVERIFY2(cursor() != cursorBefore, "Escape did not give the keyboard back to the list");
}

void TestStudio::pansTheGraphWithShiftHjk()
{
    QObject *root = m_engine->rootObjects().first();
    m_workspace->addTab(QStringLiteral("graph"));
    settle();
    QObject *graph = root->property("activeGraph").value<QObject *>();
    QVERIFY(graph);
    auto *graphItem = qobject_cast<GraphItem *>(graph);
    QVERIFY(graphItem);

    const QPointF start = graphItem->pan();
    key('h', Qt::ShiftModifier);
    const QPointF left = graphItem->pan();
    QVERIFY2(qAbs(left.x() - start.x() + 40.0) < 0.5,
             qPrintable(QStringLiteral("Shift+h pan.x was %1, expected %2")
                                .arg(left.x())
                                .arg(start.x() - 40.0)));

    key('l', Qt::ShiftModifier);
    const QPointF right = graphItem->pan();
    QVERIFY2(qAbs(right.x() - left.x() - 40.0) < 0.5,
             qPrintable(QStringLiteral("Shift+l pan.x was %1, expected %2")
                                .arg(right.x())
                                .arg(left.x() + 40.0)));

    m_workspace->closeCurrentTab();
    settle();
}

void TestStudio::resizesTheSplitWithCtrlComma()
{
    const int panesBefore = m_workspace->paneCount();
    if (panesBefore == 1)
        key(Qt::Key_Backslash, Qt::ControlModifier);
    QCOMPARE(m_workspace->paneCount(), 2);

    const int focused = m_workspace->focusedPane();
    QQuickItem *pane = nullptr;
    for (QQuickItem *item : itemsNamed(QStringLiteral("pane"))) {
        if (item->property("paneIndex").toInt() == focused) {
            pane = item;
            break;
        }
    }
    QVERIFY2(pane, "no QML pane item for the focused pane");

    const qreal before = pane->property("panePreferredWidth").toReal();
    key(Qt::Key_Period, Qt::ControlModifier);
    const qreal wider = pane->property("panePreferredWidth").toReal();
    QVERIFY2(qAbs(wider - before - 40.0) < 0.5,
             qPrintable(QStringLiteral("Ctrl+. width was %1, expected %2")
                                .arg(wider)
                                .arg(before + 40.0)));

    key(Qt::Key_Comma, Qt::ControlModifier);
    const qreal narrower = pane->property("panePreferredWidth").toReal();
    QVERIFY2(qAbs(narrower - wider + 40.0) < 0.5,
             qPrintable(QStringLiteral("Ctrl+, width was %1, expected %2")
                                .arg(narrower)
                                .arg(wider - 40.0)));
    QVERIFY2(narrower >= 280.0,
             qPrintable(QStringLiteral("Ctrl+, shrank the pane below 280: %1").arg(narrower)));

    if (panesBefore == 1) {
        key(Qt::Key_W, Qt::ControlModifier | Qt::ShiftModifier);
        QCOMPARE(m_workspace->paneCount(), 1);
    }
}

// Half the row menu is a caller of the command line, and this is the check that
// it calls it correctly: the operands are separated with `--` so a file named
// like a flag is still a file, the destructive verb carries the `--yes` the
// entry already asked for by being chosen, and the tree verbs are scoped to the
// folder in front of you rather than to the whole index.
void TestStudio::runsTheCommandLineForWhatTheRowMenuChanges()
{
    const QString file = QDir(m_tree.path()).filePath(QStringLiteral("one.txt"));
    focusRow(QStringLiteral("one.txt"));

    const struct { const char *label; QStringList expected; } cases[] = {
        {"Move to trash", {"--yes", "trash", "--", file}},
        {"Clear tags", {"tag", "clear", "--", file}},
        {"Reindex this folder", {"reindex", "--", m_tree.path()}},
        {"Prune missing rows here", {"--yes", "prune", "--", m_tree.path()}},
        {"Verify this tree", {"verify", "--", m_tree.path()}},
        {"Adopt tags from disk here", {"reindex", "--from-xattr", "--", m_tree.path()}},
    };
    for (const auto &one : cases) {
        const QString label = QString::fromLatin1(one.label);
        verbRuns().clear();
        focusRow(QStringLiteral("one.txt"));
        key('m');
        QVERIFY2(chooseInMenu(QStringLiteral("fileMenu"), label),
                 qPrintable(QStringLiteral("no menu entry called %1").arg(label)));
        QCOMPARE(verbRuns().size(), 1);
        QCOMPARE(verbRuns().first(), one.expected);
    }

    // The tag the panel is pointing at, applied by name. There is no field to
    // type into here, and that is the point of naming the tag in the label.
    verbRuns().clear();
    key('t');
    const QString tag = m_index->tagAt(0);
    QVERIFY(!tag.isEmpty());
    key(Qt::Key_Escape);
    focusRow(QStringLiteral("plain.txt"));
    const QString plain = QDir(m_tree.path()).filePath(QStringLiteral("plain.txt"));
    key('m');
    QVERIFY(chooseInMenu(QStringLiteral("fileMenu"),
                         QStringLiteral("Apply tag %1").arg(tag)));
    QCOMPARE(verbRuns().size(), 1);
    QCOMPARE(verbRuns().first(),
             (QStringList{"tag", "add", "--tag", tag, "--", plain}));

    // And the receipt lands on the one line the window has for saying things.
    QVERIFY2(m_index->notice().contains(tag),
             qPrintable(QStringLiteral("the status bar said %1").arg(m_index->notice())));
}

// The rule that keeps a delete of one file from taking forty: a command chosen
// over a marked row means the marked set, and one chosen over a row that is not
// in it means that row, whatever else is marked.
void TestStudio::actsOnTheMarkedSetOnlyFromInsideIt()
{
    const QDir tree(m_tree.path());
    focusRow(QStringLiteral("one.txt"));
    key(Qt::Key_Space);
    focusRow(QStringLiteral("two.md"));
    key(Qt::Key_Space);
    QCOMPARE(tab()->markedCount(), 2);

    verbRuns().clear();
    focusRow(QStringLiteral("one.txt"));
    key('m');
    // The count is in the label, which is what this menu has instead of a
    // dialog asking afterwards.
    QVERIFY2(chooseInMenu(QStringLiteral("fileMenu"),
                          QStringLiteral("Move 2 files to trash")),
             qPrintable(QStringLiteral("the entries were: %1")
                            .arg(menuLabels(QStringLiteral("fileMenu")).join(QLatin1String(" | ")))));
    QCOMPARE(verbRuns().size(), 1);
    // In the order the listing shows them, and not the order a hash of the
    // paths happens to give.
    QCOMPARE(verbRuns().first(),
             (QStringList{"--yes", "trash", "--",
                          tree.filePath(QStringLiteral("one.txt")),
                          tree.filePath(QStringLiteral("two.md"))}));

    verbRuns().clear();
    focusRow(QStringLiteral("three.ods"));
    key('m');
    QVERIFY(chooseInMenu(QStringLiteral("fileMenu"), QStringLiteral("Move to trash")));
    QCOMPARE(verbRuns().size(), 1);
    QCOMPARE(verbRuns().first(),
             (QStringList{"--yes", "trash", "--", tree.filePath(QStringLiteral("three.ods"))}));

    key('A');
    QCOMPARE(tab()->markedCount(), 0);
}

void TestStudio::copiesWhatTheRowIsToTheClipboard()
{
    const QString file = QDir(m_tree.path()).filePath(QStringLiteral("one.txt"));
    focusRow(QStringLiteral("one.txt"));

    key('m');
    QVERIFY(chooseInMenu(QStringLiteral("fileMenu"), QStringLiteral("Copy path")));
    QCOMPARE(m_clipboard->lastCopied(), file);

    key('m');
    QVERIFY(chooseInMenu(QStringLiteral("fileMenu"), QStringLiteral("Copy name")));
    QCOMPARE(m_clipboard->lastCopied(), QStringLiteral("one.txt"));

    key('m');
    QVERIFY(chooseInMenu(QStringLiteral("fileMenu"), QStringLiteral("Copy as URI")));
    QCOMPARE(m_clipboard->lastCopied(), QStringLiteral("file://") + file);

    key('m');
    QVERIFY(chooseInMenu(QStringLiteral("fileMenu"), QStringLiteral("Copy tags")));
    QCOMPARE(m_clipboard->lastCopied(), QStringLiteral("notes work"));

    // The tag is left out of the command on purpose: it is copied to be
    // finished, and one guessed here would be one applied by somebody who meant
    // to type a different one.
    key('m');
    QVERIFY(chooseInMenu(QStringLiteral("fileMenu"), QStringLiteral("Copy tag command")));
    QCOMPARE(m_clipboard->lastCopied(),
             QStringLiteral("omafiles tag add --tag  -- ") + file);

    // A row with nothing on it has no tags to copy, so the entry is not there
    // to be chosen rather than there and doing nothing.
    focusRow(QStringLiteral("plain.txt"));
    key('m');
    QVERIFY(!menuLabels(QStringLiteral("fileMenu")).contains(QStringLiteral("Copy tags")));
    key(Qt::Key_Escape);
}

// A tag listing is not a place in the tree: its rows live in as many folders as
// they like, and there is a way from one of them to the folder it is really in.
void TestStudio::walksToTheFolderHoldingATaggedFile()
{
    const QString file = QDir(m_tree.path()).filePath(QStringLiteral("sweep/one.txt"));
    tagDirectory(file, QStringLiteral("sweep-tag"));
    tab()->setCwd(m_tree.path());
    tab()->setSelectedTag(QStringLiteral("sweep-tag"));
    settle();
    QCOMPARE(cwd(), m_tree.path());

    const int row = rowOf(QStringLiteral("one.txt"));
    QVERIFY2(row >= 0, "the tag listing did not hold the tagged file");
    QMetaObject::invokeMethod(filesRoot(), "setCursor", Q_ARG(QVariant, QVariant(row)));
    settle();

    key('m');
    QVERIFY(chooseInMenu(QStringLiteral("fileMenu"),
                         QStringLiteral("Go to containing folder")));
    settle();
    QCOMPARE(cwd(), QDir(m_tree.path()).filePath(QStringLiteral("sweep")));
    // Landed on the file and not at the top of the folder: walking to a file is
    // asking about that file.
    QCOMPARE(tab()->selectedTag(), QString());
    QCOMPARE(rowOf(QStringLiteral("one.txt")), cursor());
}

void TestStudio::marksBySameTagSameTypeAndTheOtherWayRound()
{
    QObject *folder = filesRoot()->property("folder").value<QObject *>();
    QVERIFY(folder);

    // `one.txt` carries `notes` and `work`; so does `six.md`, and `two.md`
    // carries `notes`. Sharing any one tag is enough.
    focusRow(QStringLiteral("one.txt"));
    key('m');
    QVERIFY(chooseInMenu(QStringLiteral("fileMenu"), QStringLiteral("Mark same tag")));
    QStringList marked;
    QMetaObject::invokeMethod(folder, "markedPaths", Q_RETURN_ARG(QStringList, marked));
    QVERIFY2(marked.size() >= 3, qPrintable(marked.join(QLatin1String(" "))));
    for (const QString &name : {QStringLiteral("one.txt"), QStringLiteral("two.md"),
                                QStringLiteral("six.md")}) {
        QVERIFY2(marked.contains(QDir(m_tree.path()).filePath(name)),
                 qPrintable(QStringLiteral("%1 was not marked").arg(name)));
    }
    QVERIFY(!marked.contains(QDir(m_tree.path()).filePath(QStringLiteral("five.zip"))));

    key('A');
    focusRow(QStringLiteral("one.txt"));
    key('m');
    QVERIFY(chooseInMenu(QStringLiteral("fileMenu"), QStringLiteral("Mark same type")));
    QMetaObject::invokeMethod(folder, "markedPaths", Q_RETURN_ARG(QStringList, marked));
    for (const QString &name : {QStringLiteral("one.txt"), QStringLiteral("seven.txt"),
                                QStringLiteral("plain.txt")}) {
        QVERIFY2(marked.contains(QDir(m_tree.path()).filePath(name)),
                 qPrintable(QStringLiteral("%1 was not marked").arg(name)));
    }
    QVERIFY(!marked.contains(QDir(m_tree.path()).filePath(QStringLiteral("two.md"))));

    // Inverting takes the folders too, and leaves `..` where it is: it is a way
    // up and not a row anything should act on.
    const int before = tab()->markedCount();
    int rows = 0;
    for (int i = 0; i < folder->property("count").toInt(); ++i) {
        QString kind;
        QMetaObject::invokeMethod(folder, "kindAt", Q_RETURN_ARG(QString, kind), Q_ARG(int, i));
        if (kind != QLatin1String("parent"))
            ++rows;
    }
    focusRow(QStringLiteral("one.txt"));
    key('m');
    QVERIFY(chooseInMenu(QStringLiteral("fileMenu"), QStringLiteral("Invert marks")));
    QCOMPARE(tab()->markedCount(), rows - before);
    QMetaObject::invokeMethod(folder, "markedPaths", Q_RETURN_ARG(QStringList, marked));
    QVERIFY(!marked.contains(QDir(m_tree.path()).filePath(QStringLiteral("one.txt"))));
    key('A');
}

void TestStudio::worksTheTabMenu()
{
    PaneState *pane = m_workspace->paneAtIndex(0);
    QVERIFY(pane);
    tab()->setCwd(QDir(m_tree.path()).filePath(QStringLiteral("alpha")));
    settle();

    QMetaObject::invokeMethod(m_engine->rootObjects().first(), "openTabMenuAt",
                              Q_ARG(QVariant, QVariant(0)), Q_ARG(QVariant, QVariant(0)),
                              Q_ARG(QVariant, QVariant(40)), Q_ARG(QVariant, QVariant(40)));
    settle();
    QVERIFY(chooseInMenu(QStringLiteral("tabMenu"), QStringLiteral("Duplicate tab")));
    settle();
    QCOMPARE(pane->tabCount(), 2);
    // The same folder, and its own marks: a duplicate is a second view of a
    // place, not a second copy of a set somebody assembled by hand.
    QCOMPARE(pane->tabList().at(1)->cwd(),
             QDir(m_tree.path()).filePath(QStringLiteral("alpha")));
    QCOMPARE(pane->tabList().at(1)->markedCount(), 0);

    QMetaObject::invokeMethod(m_engine->rootObjects().first(), "openTabMenuAt",
                              Q_ARG(QVariant, QVariant(0)), Q_ARG(QVariant, QVariant(0)),
                              Q_ARG(QVariant, QVariant(40)), Q_ARG(QVariant, QVariant(40)));
    settle();
    QVERIFY(chooseInMenu(QStringLiteral("tabMenu"), QStringLiteral("Close tabs to the right")));
    settle();
    QCOMPARE(pane->tabCount(), 1);

    // A pane that would be emptied keeps its tab, so the entry that would empty
    // it is not drawn at all.
    m_workspace->splitRight(QStringLiteral("files"));
    settle();
    QMetaObject::invokeMethod(m_engine->rootObjects().first(), "openTabMenuAt",
                              Q_ARG(QVariant, QVariant(0)), Q_ARG(QVariant, QVariant(0)),
                              Q_ARG(QVariant, QVariant(40)), Q_ARG(QVariant, QVariant(40)));
    settle();
    QVERIFY(!menuLabels(QStringLiteral("tabMenu"))
                 .contains(QStringLiteral("Move tab to other pane")));
    key(Qt::Key_Escape);

    m_workspace->addTabIn(0, QStringLiteral("files"));
    settle();
    QMetaObject::invokeMethod(m_engine->rootObjects().first(), "openTabMenuAt",
                              Q_ARG(QVariant, QVariant(0)), Q_ARG(QVariant, QVariant(0)),
                              Q_ARG(QVariant, QVariant(40)), Q_ARG(QVariant, QVariant(40)));
    settle();
    QVERIFY(chooseInMenu(QStringLiteral("tabMenu"), QStringLiteral("Move tab to other pane")));
    settle();
    QCOMPARE(m_workspace->paneAtIndex(0)->tabCount(), 1);
    QCOMPARE(m_workspace->paneAtIndex(1)->tabCount(), 2);
    QCOMPARE(m_workspace->focusedPane(), 1);

    while (m_workspace->paneCount() > 1)
        m_workspace->closePane(m_workspace->paneCount() - 1);
    settle();
}

// `Open with` is a second page of the same menu, and this is the reader behind
// it: the associations the desktop keeps, out of the files the desktop keeps
// them in. Written into the fixture rather than read off this machine, so the
// case says the same thing on a machine with no applications installed at all.
void TestStudio::offersTheApplicationsRegisteredForTheFile()
{
    const QDir data(m_home.path());
    QVERIFY(data.mkpath(QStringLiteral("applications")));
    QDir apps(data.filePath(QStringLiteral("applications")));

    const struct { const char *file; const char *body; } entries[] = {
        {"oma-reader.desktop",
         "[Desktop Entry]\nType=Application\nName=Oma Reader\nExec=oma-reader %f\n"},
        {"oma-hidden.desktop",
         "[Desktop Entry]\nType=Application\nName=Oma Hidden\nNoDisplay=true\nExec=x %f\n"},
        {"oma-dropped.desktop",
         "[Desktop Entry]\nType=Application\nName=Oma Dropped\nExec=x %f\n"},
    };
    for (const auto &entry : entries) {
        QFile file(apps.filePath(QString::fromLatin1(entry.file)));
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Text));
        file.write(entry.body);
    }

    QFile cache(apps.filePath(QStringLiteral("mimeinfo.cache")));
    QVERIFY(cache.open(QIODevice::WriteOnly | QIODevice::Text));
    cache.write("[MIME Cache]\n"
                "text/plain=oma-reader.desktop;oma-hidden.desktop;oma-dropped.desktop;\n");
    cache.close();

    // The user's own list, which is where a removal is written when somebody
    // takes an application off a type in their desktop settings.
    QFile associations(apps.filePath(QStringLiteral("mimeapps.list")));
    QVERIFY(associations.open(QIODevice::WriteOnly | QIODevice::Text));
    associations.write("[Removed Associations]\ntext/plain=oma-dropped.desktop;\n");
    associations.close();

    const QString file = QDir(m_tree.path()).filePath(QStringLiteral("one.txt"));
    QStringList names;
    for (const QVariant &app : m_index->applicationsFor(file))
        names.append(app.toMap().value(QStringLiteral("name")).toString());

    QVERIFY2(names.contains(QStringLiteral("Oma Reader")), qPrintable(names.join(QLatin1String(" | "))));
    // What the desktop menu leaves out, this leaves out too.
    QVERIFY2(!names.contains(QStringLiteral("Oma Hidden")), "a NoDisplay entry was offered");
    QVERIFY2(!names.contains(QStringLiteral("Oma Dropped")), "a removed association was offered");

    // And the entry file is what gets launched, not the name: two applications
    // can be called the same thing.
    focusRow(QStringLiteral("one.txt"));
    key('m');
    QVERIFY(chooseInMenu(QStringLiteral("fileMenu"), QStringLiteral("Open with…")));
    settle();
    QVERIFY2(menuLabels(QStringLiteral("openWithMenu")).contains(QStringLiteral("Oma Reader")),
             qPrintable(menuLabels(QStringLiteral("openWithMenu")).join(QLatin1String(" | "))));

    openRequests().clear();
    QVERIFY(chooseInMenu(QStringLiteral("openWithMenu"), QStringLiteral("Oma Reader")));
    QCOMPARE(openRequests().size(), 1);
    QVERIFY2(openRequests().first().kind == OpenRequest::OpenWith,
             "choosing an application asked for something else");
    QCOMPARE(openRequests().first().path, file);
    QCOMPARE(openRequests().first().application,
             apps.filePath(QStringLiteral("oma-reader.desktop")));

    QVERIFY(apps.removeRecursively());
}

// The tab where a tag is a folder.
//
// The fixture's vocabulary is what is being read: `one.txt` carries `notes
// work`, `six.md` carries `notes project work`, `two.md` carries `notes`, and
// `four.drawio` carries `diagram project`. So `#notes` holds three files and
// still divides into `work` and `project`, and `#notes/#work` holds two.
void TestStudio::walksTagsAsIfTheyWereFolders()
{
    m_workspace->addTab(QStringLiteral("tags"));
    settle();
    QCOMPARE(tab()->kind(), QStringLiteral("tags"));
    QCOMPARE(tab()->title(), QStringLiteral("Tags"));

    QObject *folder = filesRoot()->property("folder").value<QObject *>();
    QVERIFY(folder);

    const auto namesOfKind = [folder](const QString &wanted) {
        QStringList names;
        for (int i = 0; i < folder->property("count").toInt(); ++i) {
            QVariantMap entry;
            QMetaObject::invokeMethod(folder, "entryAt", Q_RETURN_ARG(QVariantMap, entry),
                                      Q_ARG(int, i));
            if (entry.value(QStringLiteral("kind")).toString() == wanted)
                names.append(entry.value(QStringLiteral("name")).toString());
        }
        return names;
    };

    // The root narrows nothing, so it holds the whole vocabulary and every row
    // the index knows about -- the answer `omafiles ls` gives with no filter on
    // it, which is the count the header shows. Counted against that rather than
    // against a number written here: earlier cases in this file tag paths of
    // their own, and a literal would be a case that only passes alone.
    QStringList facets = namesOfKind(QStringLiteral("tag"));
    QVERIFY2(facets.contains(QStringLiteral("notes")), qPrintable(facets.join(QLatin1String(" "))));
    QVERIFY2(facets.contains(QStringLiteral("archive")), qPrintable(facets.join(QLatin1String(" "))));
    int rowsWithAPath = 0;
    for (int i = 0; i < folder->property("count").toInt(); ++i) {
        QString path;
        QMetaObject::invokeMethod(folder, "pathAt", Q_RETURN_ARG(QString, path), Q_ARG(int, i));
        if (!path.isEmpty())
            ++rowsWithAPath;
    }
    QCOMPARE(rowsWithAPath, m_index->fileCount());

    // Walking into one narrows the question rather than moving anywhere.
    focusRow(QStringLiteral("notes"));
    key('l');
    settle();
    QCOMPARE(tab()->facets(), QStringList{QStringLiteral("notes")});
    QCOMPARE(tab()->title(), QStringLiteral("#notes"));
    QCOMPARE(namesOfKind(QStringLiteral("file")).size(), 3);

    facets = namesOfKind(QStringLiteral("tag"));
    QVERIFY2(facets.contains(QStringLiteral("work")), qPrintable(facets.join(QLatin1String(" "))));
    // A tag no file here carries is not offered: it would be a folder that
    // opens empty.
    QVERIFY2(!facets.contains(QStringLiteral("archive")),
             "a tag with nothing under it was offered as a folder");
    // And neither is the one already walked into: `#notes/#notes` is `#notes`.
    QVERIFY2(!facets.contains(QStringLiteral("notes")),
             "the tag already in the path was offered again");

    focusRow(QStringLiteral("work"));
    key('l');
    settle();
    QCOMPARE(tab()->facets(), (QStringList{QStringLiteral("notes"), QStringLiteral("work")}));
    QCOMPARE(namesOfKind(QStringLiteral("file")).size(), 2);
    // Both files here carry `work` and `notes`, so neither tells them apart any
    // more -- and one that divides nothing is a folder onto the same list.
    QVERIFY2(!namesOfKind(QStringLiteral("tag")).contains(QStringLiteral("notes")),
             "a tag every file here carries was offered as a folder");

    // `h` takes the last tag off, and `~` takes them all off: the top of this
    // tab is the vocabulary, the way the top of a folder tab is home.
    key('h');
    QCOMPARE(tab()->facets(), QStringList{QStringLiteral("notes")});
    key('~');
    QCOMPARE(tab()->facets(), QStringList());

    m_workspace->closeCurrentTab();
    settle();
}

// The property a directory tree does not have: a file is under every path that
// describes it, so the same tags in another order are the same listing and not
// a second one.
void TestStudio::asksOneQuestionWhicheverWayItIsWalked()
{
    m_workspace->addTab(QStringLiteral("tags"));
    settle();
    QObject *folder = filesRoot()->property("folder").value<QObject *>();
    QVERIFY(folder);

    const auto listing = [folder] {
        QStringList paths;
        for (int i = 0; i < folder->property("count").toInt(); ++i) {
            QString path;
            QMetaObject::invokeMethod(folder, "pathAt", Q_RETURN_ARG(QString, path),
                                      Q_ARG(int, i));
            if (!path.isEmpty())
                paths.append(path);
        }
        paths.sort();
        return paths;
    };

    tab()->setFacets({QStringLiteral("notes"), QStringLiteral("work")});
    settle();
    const QStringList oneWay = listing();
    QCOMPARE(oneWay.size(), 2);
    const QString title = tab()->title();

    tab()->setFacets({QStringLiteral("work"), QStringLiteral("notes")});
    settle();
    QCOMPARE(listing(), oneWay);
    // And it is called the same thing either way, or one question would look
    // like two in the tab strip.
    QCOMPARE(tab()->title(), title);

    // A facet row carries no path, so nothing that acts on a file can reach
    // one: marking everything marks the files and never the tags.
    tab()->setFacets({});
    settle();
    QMetaObject::invokeMethod(folder, "markAll");
    QCOMPARE(tab()->markedCount(), listing().size());
    QStringList marked;
    QMetaObject::invokeMethod(folder, "markedPaths", Q_RETURN_ARG(QStringList, marked));
    QVERIFY2(!marked.contains(QString()), "a tag row ended up in the marked set");

    m_workspace->closeCurrentTab();
    settle();
}

// The menu answers the mouse as well as the keyboard. Qt Quick delivers a press
// to a child only inside its ancestors, so the scope holding the panel needs a
// size; keyboard focus does not care about geometry.
void TestStudio::choosesARowMenuEntryWithTheMouse()
{
    const QString file = QDir(m_tree.path()).filePath(QStringLiteral("one.txt"));
    QTest::mouseMove(window(), QPoint(window()->width() - 16, window()->height() - 16));
    focusRow(QStringLiteral("one.txt"));
    key('m');
    QObject *root = m_engine->rootObjects().first();
    QVERIFY2(root->property("blocked").toBool(), "m did not open the row menu");

    const int at = menuLabels(QStringLiteral("fileMenu")).indexOf(QStringLiteral("Copy path"));
    QVERIFY(at >= 0);

    QQuickItem *popup = findAccessibleItem(window()->contentItem(), QAccessible::PopupMenu,
                                           QStringLiteral("Menu"));
    QVERIFY2(popup, "no row menu popup on screen");
    QQuickItem *list = findAccessibleItem(popup, QAccessible::List);
    QVERIFY(list);
    QQuickItem *entry = viewItemAt(list, at);
    QVERIFY2(entry, "the entry to click has not been built");

    const QPointF centre = entry->mapToScene(QPointF(entry->width() / 2, entry->height() / 2));
    m_clipboard->copy(QString());
    QTest::mouseMove(window(), centre.toPoint());
    settle();
    // Hovering moves the highlight, which is the whole of what a pointer
    // expects from a menu: the entry under it lights up before it is clicked.
    QCOMPARE(list->property("currentIndex").toInt(), at);
    QTest::mouseClick(window(), Qt::LeftButton, Qt::NoModifier, centre.toPoint());
    settle();

    QCOMPARE(m_clipboard->lastCopied(), file);
    QCOMPARE(root->property("blocked").toBool(), false);

    // And again by the road a person actually takes: the menu opened by a
    // right-click, so the pointer is already inside the panel when it appears.
    QQuickItem *listing = qobject_cast<QQuickItem *>(
        filesRoot()->property("view").value<QObject *>());
    QVERIFY(listing);
    QQuickItem *row = viewItemAt(listing, rowOf(QStringLiteral("one.txt")));
    QVERIFY(row);
    const QPointF onRow = row->mapToScene(QPointF(row->width() / 2, row->height() / 2));
    QTest::mouseClick(window(), Qt::RightButton, Qt::NoModifier, onRow.toPoint());
    settle();
    QVERIFY2(root->property("blocked").toBool(), "a right-click opened no menu");

    QQuickItem *reopened = findAccessibleItem(window()->contentItem(), QAccessible::PopupMenu,
                                              QStringLiteral("Menu"));
    QVERIFY(reopened);
    QQuickItem *rows = findAccessibleItem(reopened, QAccessible::List);
    QVERIFY(rows);
    const int second =
        menuLabels(QStringLiteral("fileMenu")).indexOf(QStringLiteral("Copy name"));
    QVERIFY(second >= 0);
    QQuickItem *pick = viewItemAt(rows, second);
    QVERIFY2(pick, "the entry to click has not been built");
    const QPointF spot = pick->mapToScene(QPointF(pick->width() / 2, pick->height() / 2));
    m_clipboard->copy(QString());
    QTest::mouseMove(window(), spot.toPoint());
    settle();
    QTest::mouseClick(window(), Qt::LeftButton, Qt::NoModifier, spot.toPoint());
    settle();
    QCOMPARE(m_clipboard->lastCopied(), QStringLiteral("one.txt"));

    // The pointer is left where the last case put it, and the menu answers it
    // now, so parking it off the window is how this one stops deciding what
    // the next one's menu opens on.
    QTest::mouseMove(window(), QPoint(int(window()->width() * 0.35),
                                      int(window()->height() * 0.85)));
    settle();
}

// A folder, a tag and a query are one list, and one keystroke opens whichever
// kind the row turned out to be.
void TestStudio::opensAndSavesAView()
{
    QObject *root = m_engine->rootObjects().first();
    QVERIFY2(m_views->rowCount() >= 2, "the fixture saved no views");

    QStringList names;
    for (int i = 0; i < m_views->rowCount(); ++i)
        names.append(m_views->at(i).value(QStringLiteral("name")).toString());
    QVERIFY2(names.contains(QStringLiteral("the notes")), qPrintable(names.join(QLatin1String(" "))));

    // `v` walks the panel, `h` leaves it the way it leaves the tag list.
    key('v');
    QCOMPARE(root->property("focusRegion").toString(), QStringLiteral("views"));
    key('h');
    QCOMPARE(root->property("focusRegion").toString(), QStringLiteral("list"));

    // A tag view opens as a tags tab standing on its query.
    const int tagged = names.indexOf(QStringLiteral("the notes"));
    QMetaObject::invokeMethod(root, "openView", Q_ARG(QVariant, QVariant(tagged)));
    settle();
    QCOMPARE(tab()->kind(), QStringLiteral("tags"));
    QCOMPARE(tab()->facets(), QStringList{QStringLiteral("notes")});

    // And it is saved by the same road it is opened: what is on screen, by a
    // name, through the command line the shell would have used.
    verbRuns().clear();
    QMetaObject::invokeMethod(root, "startNaming");
    settle();
    QVERIFY2(root->property("naming").toBool(), "nothing asked for a name");
    QMetaObject::invokeMethod(root, "finishNaming", Q_ARG(QVariant, QVariant(true)),
                              Q_ARG(QVariant, QVariant(QStringLiteral("my notes"))));
    settle();
    QCOMPARE(verbRuns().size(), 1);
    QCOMPARE(verbRuns().first(),
             (QStringList{"view", "add", "--name", "my notes", "--tag", "notes"}));
    m_workspace->closeCurrentTab();
    settle();

    // A folder view opens as a folder.
    const int folder = names.indexOf(QStringLiteral("alpha"));
    QMetaObject::invokeMethod(root, "openView", Q_ARG(QVariant, QVariant(folder)));
    settle();
    QCOMPARE(tab()->kind(), QStringLiteral("files"));
    QCOMPARE(cwd(), QDir(m_tree.path()).filePath(QStringLiteral("alpha")));

    verbRuns().clear();
    QMetaObject::invokeMethod(root, "startNaming");
    settle();
    QMetaObject::invokeMethod(root, "finishNaming", Q_ARG(QVariant, QVariant(true)),
                              Q_ARG(QVariant, QVariant(QStringLiteral("inner"))));
    settle();
    QCOMPARE(verbRuns().size(), 1);
    QCOMPARE(verbRuns().first(),
             (QStringList{"view", "add", "--name", "inner", "--",
                          QDir(m_tree.path()).filePath(QStringLiteral("alpha"))}));

    m_workspace->closeCurrentTab();
    settle();

    // A view is a row like any other row: `m` over it opens a menu, and the
    // menu is how one is taken off the panel. Without it a view could be made
    // from the window and only unmade from a prompt.
    key('v');
    QCOMPARE(root->property("focusRegion").toString(), QStringLiteral("views"));
    const int before = m_views->rowCount();
    key('m');
    QVERIFY2(root->property("blocked").toBool(), "m over a view opened nothing");
    QVERIFY2(menuLabels(QStringLiteral("viewMenu")).contains(QStringLiteral("Copy the command")),
             qPrintable(menuLabels(QStringLiteral("viewMenu")).join(QLatin1String(" | "))));

    m_clipboard->copy(QString());
    QVERIFY(chooseInMenu(QStringLiteral("viewMenu"), QStringLiteral("Copy the command")));
    QVERIFY2(m_clipboard->lastCopied().startsWith(QStringLiteral("omafiles ls")),
             qPrintable(m_clipboard->lastCopied()));

    // And forgetting one goes out through the command line, like every other
    // change this window makes.
    verbRuns().clear();
    key('v');
    key('m');
    const QString first = m_views->at(0).value(QStringLiteral("name")).toString();
    // The keyboard has to still be there after the menu closed: the menu takes
    // the focus and hands it back, and handing back the region without the
    // focus left the window answering nothing at all.
    QVERIFY2(root->property("blocked").toBool(), "m over a view opened nothing the second time");
    QVERIFY(chooseInMenu(QStringLiteral("viewMenu"), QStringLiteral("Forget ") + first));
    QCOMPARE(verbRuns().size(), 1);
    QCOMPARE(verbRuns().first(), (QStringList{"view", "rm", "--name", first}));
    // The recorder answers without writing anything, so the panel still holds
    // what it held: what this checks is the command, not the removal.
    QCOMPARE(m_views->rowCount(), before);

    key(Qt::Key_Escape);
    settle();
}

// The other place the root of a tags tab leads to: what carries no tag at all.
//
// The index cannot answer this, and that is the whole difficulty. It holds rows
// that carry a tag, so what has no tag has no row, and the exact answer is a
// walk of the tree on every question -- which is not something a panel somebody
// is typing into can afford. So the answer comes out of the list of names
// `sweep` writes, and the price of that is a listing as old as the last sweep.
// The price has to be visible, which is what the status line is measured for
// here.
//
// Both halves are written by hand rather than swept: nothing in this binary may
// run the command line.
void TestStudio::walksIntoWhatCarriesNoTag()
{
    QTemporaryDir cache;
    QTemporaryDir shelf;
    QVERIFY(cache.isValid());
    QVERIFY(shelf.isValid());

    const QDir room(shelf.path());
    for (const char *name : {"filed.pdf", "unfiled.pdf", "later.pdf"}) {
        QFile file(room.filePath(QString::fromLatin1(name)));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("x");
    }

    const QByteArray previousCache = qgetenv("XDG_CACHE_HOME");
    qputenv("XDG_CACHE_HOME", cache.path().toUtf8());
    const QVector<Root> previousRoots = declareRoots({});
    const auto restore = qScopeGuard([&previousCache, &previousRoots] {
        qputenv("XDG_CACHE_HOME", previousCache);
        declareRoots(previousRoots);
    });

    tagDirectory(room.filePath(QStringLiteral("filed.pdf")), QStringLiteral("filed"));

    m_workspace->addTab(QStringLiteral("tags"));
    settle();
    QObject *folder = filesRoot()->property("folder").value<QObject *>();
    QVERIFY(folder);
    const auto namesOfKind = [folder](const QString &wanted) {
        QStringList names;
        for (int i = 0; i < folder->property("count").toInt(); ++i) {
            QVariantMap entry;
            QMetaObject::invokeMethod(folder, "entryAt", Q_RETURN_ARG(QVariantMap, entry),
                                      Q_ARG(int, i));
            if (entry.value(QStringLiteral("kind")).toString() == wanted)
                names.append(entry.value(QStringLiteral("name")).toString());
        }
        return names;
    };

    // The node is at the root of the vocabulary and nowhere else, and it has no
    // path -- so nothing that acts on a file can reach it, the way a facet row
    // cannot be reached either.
    QCOMPARE(namesOfKind(QStringLiteral("untagged")),
             QStringList{QStringLiteral("(no tag)")});
    const int node = rowOf(QStringLiteral("(no tag)"));
    QVERIFY(node >= 0);
    QString path;
    QMetaObject::invokeMethod(folder, "pathAt", Q_RETURN_ARG(QString, path), Q_ARG(int, node));
    QVERIFY2(path.isEmpty(), "the complement row carried a path");

    // It is offered on a machine that has declared nothing too, and opening it
    // there says what a scope for the question would be. A feature invisible
    // until you already use it is one nobody finds.
    focusRow(QStringLiteral("(no tag)"));
    key('l');
    settle();
    QVERIFY(tab()->untagged());
    QCOMPARE(tab()->title(), QStringLiteral("(no tag)"));
    QVERIFY2(folder->property("error").toString().contains(QStringLiteral("roots add")),
             qPrintable(folder->property("error").toString()));

    // Declared, and nothing has swept: the tab says that rather than drawing an
    // empty folder, which would read as "everything is filed".
    declareRoots({Root{QStringLiteral("shelf"), room.absolutePath(), false, QString()}});
    QMetaObject::invokeMethod(folder, "reload");
    settle();
    QVERIFY2(folder->property("error").toString().contains(QStringLiteral("no list yet")),
             qPrintable(folder->property("error").toString()));

    // With one, the listing is the list of names minus the index: `filed.pdf`
    // has a row and the other two do not.
    const QDir names(QDir(cache.path()).filePath(QStringLiteral("omafiles/names")));
    QVERIFY(QDir().mkpath(names.absolutePath()));
    QFile list(names.filePath(QStringLiteral("shelf")));
    QVERIFY(list.open(QIODevice::WriteOnly | QIODevice::Text));
    list.write("filed.pdf\nunfiled.pdf\n");   // `later.pdf` arrived after the sweep
    list.close();
    QMetaObject::invokeMethod(folder, "reload");
    settle();

    QCOMPARE(namesOfKind(QStringLiteral("file")), QStringList{QStringLiteral("unfiled.pdf")});
    // Said out loud, because a listing that is quietly out of date cannot be
    // judged and one that says its age can: `later.pdf` is on the disk and not
    // in the answer, and the line on screen is what explains why.
    const QString said = folder->property("error").toString();
    QVERIFY2(said.contains(QStringLiteral("as of ")), qPrintable(said));

    // Out of it is back to the vocabulary, the same one step up that leaving a
    // tag is -- and walking into a tag from there leaves the complement, since
    // a file carrying `#filed` is not one of the files carrying nothing.
    key('h');
    settle();
    QVERIFY(!tab()->untagged());
    QCOMPARE(tab()->title(), QStringLiteral("Tags"));
    focusRow(QStringLiteral("(no tag)"));
    key('l');
    settle();
    QVERIFY(tab()->untagged());
    tab()->setFacets(QStringList{QStringLiteral("filed")});
    QVERIFY2(!tab()->untagged(), "a facet and the complement were both on at once");

    m_workspace->closeCurrentTab();
    settle();
}

// A rename is `mv` to the same folder, asked for in the bar the filter uses; a
// new folder is the one thing the window makes without the command line.
void TestStudio::renamesAndMakesAFolderFromThePromptBar()
{
    QObject *root = m_engine->rootObjects().first();
    const QDir tree(m_tree.path());
    const QString one = tree.filePath(QStringLiteral("one.txt"));
    const auto answer = [&](const char *prompt, const QString &text) {
        QMetaObject::invokeMethod(root, prompt, Q_ARG(QVariant, QVariant(true)),
                                  Q_ARG(QVariant, QVariant(text)));
        settle();
    };

    // `r` asks, with the name in the box, and Escape is no rename.
    focusRow(QStringLiteral("one.txt"));
    key('r');
    QCOMPARE(root->property("prompt").toString(), QStringLiteral("rename"));
    QVERIFY(root->property("blocked").toBool());
    QObject *entry = root->findChild<QObject *>(QStringLiteral("filterEntry"));
    QVERIFY(entry);
    QCOMPARE(entry->property("text").toString(), QStringLiteral("one.txt"));
    verbRuns().clear();
    key(Qt::Key_Escape);
    QCOMPARE(root->property("prompt").toString(), QString());
    QVERIFY(verbRuns().isEmpty());

    // Committed: the command line the shell would have been handed, and the
    // cursor pointed at the name that is about to exist.
    key(Qt::Key_F2);
    QCOMPARE(root->property("prompt").toString(), QStringLiteral("rename"));
    answer("finishRename", QStringLiteral("uno.txt"));
    QCOMPARE(verbRuns().size(), 1);
    QCOMPARE(verbRuns().first(),
             (QStringList{"mv", "--", one, tree.filePath(QStringLiteral("uno.txt"))}));
    QCOMPARE(tab()->cursorPath(), tree.filePath(QStringLiteral("uno.txt")));
    QCOMPARE(root->property("blocked").toBool(), false);
    QCOMPARE(root->property("focusRegion").toString(), QStringLiteral("list"));

    // The same name is not a rename, and a name that is taken is refused in
    // `mv`'s words before `mv` could move the file into the folder of that name.
    verbRuns().clear();
    focusRow(QStringLiteral("one.txt"));
    key('r');
    answer("finishRename", QStringLiteral("one.txt"));
    QVERIFY(verbRuns().isEmpty());
    key('r');
    answer("finishRename", QStringLiteral("alpha"));
    QVERIFY(verbRuns().isEmpty());
    QVERIFY2(m_index->notice().contains(QStringLiteral("already exists")),
             qPrintable(m_index->notice()));

    // The `..` row has no name to change.
    QMetaObject::invokeMethod(filesRoot(), "setCursor", Q_ARG(QVariant, QVariant(0)));
    settle();
    key('r');
    QCOMPARE(root->property("prompt").toString(), QString());

    // A marked set is renamed by a template, through the verb that settles
    // every name before it moves any; the template is what the box asks for.
    focusRow(QStringLiteral("one.txt"));
    key(Qt::Key_Space);
    focusRow(QStringLiteral("two.md"));
    key(Qt::Key_Space);
    verbRuns().clear();
    key('r');
    QCOMPARE(root->property("prompt").toString(), QStringLiteral("set"));
    QCOMPARE(entry->property("text").toString(), QStringLiteral("{stem}{ext}"));
    answer("finishRenameSet", QStringLiteral("{stem}-{n}{ext}"));
    QCOMPARE(verbRuns().size(), 1);
    QCOMPARE(verbRuns().first(),
             (QStringList{"mv", "--pattern", "{stem}-{n}{ext}", "--", one,
                          tree.filePath(QStringLiteral("two.md"))}));
    // Left as it was offered is no rename.
    key('r');
    answer("finishRenameSet", QStringLiteral("{name}"));
    QCOMPARE(verbRuns().size(), 1);
    key('A');

    // A folder is made where the list stands, listed at once, and under the
    // cursor; made twice, it is refused with the system's reason.
    key('n');
    QCOMPARE(root->property("prompt").toString(), QStringLiteral("folder"));
    QCOMPARE(entry->property("text").toString(), QString());
    answer("finishMakingFolder", QStringLiteral("fresh"));
    QVERIFY(QFileInfo(tree.filePath(QStringLiteral("fresh"))).isDir());
    QCOMPARE(m_index->notice(), QStringLiteral("made fresh"));
    QCOMPARE(cursor(), rowOf(QStringLiteral("fresh")));
    key('n');
    answer("finishMakingFolder", QStringLiteral("fresh"));
    QVERIFY2(m_index->notice().startsWith(QStringLiteral("mkdir: cannot make")),
             qPrintable(m_index->notice()));

    focusRow(QStringLiteral("one.txt"));
    key('m');
    const QStringList labels = menuLabels(QStringLiteral("fileMenu"));
    QVERIFY(labels.contains(QStringLiteral("Rename")));
    QVERIFY(labels.contains(QStringLiteral("New folder")));
    key(Qt::Key_Escape);

    // A tags tab spans the index, so there is nowhere in it to put a folder.
    m_workspace->addTab(QStringLiteral("tags"));
    settle();
    key('n');
    QCOMPARE(root->property("prompt").toString(), QString());
    m_workspace->closeCurrentTab();
    settle();

    QVERIFY(tree.rmdir(QStringLiteral("fresh")));
    settle();
}

// Through the real clipboard, in the formats Nautilus and Dolphin read, and
// pasted as the `cp` and `mv` the shell would have been handed.
void TestStudio::copiesCutsAndPastesThroughTheDesktopClipboard()
{
    QObject *root = m_engine->rootObjects().first();
    const QDir tree(m_tree.path());
    const QString one = tree.filePath(QStringLiteral("one.txt"));
    const QString two = tree.filePath(QStringLiteral("two.md"));
    const QString alpha = tree.filePath(QStringLiteral("alpha"));

    // Copy takes the row; the desktop sees files, and text for anything that
    // only takes text.
    m_clipboard->clear();
    settle();
    QVERIFY(!m_clipboard->holdsFiles());
    focusRow(QStringLiteral("one.txt"));
    key('c', Qt::ControlModifier);
    QVERIFY(m_clipboard->holdsFiles());
    QCOMPARE(m_clipboard->files(), QStringList{one});
    QVERIFY(!m_clipboard->cutting());
    QCOMPARE(m_clipboard->lastCopied(), one);
    const QMimeData *data = QGuiApplication::clipboard()->mimeData();
    QVERIFY(data && data->hasUrls());
    QCOMPARE(data->data(QStringLiteral("x-special/gnome-copied-files")),
             QByteArray("copy\n") + QUrl::fromLocalFile(one).toString(QUrl::FullyEncoded).toUtf8());

    // Pasted into a folder, it is `cp` into that folder, and the cursor is
    // pointed at the copy.
    verbRuns().clear();
    tab()->setCwd(alpha);
    settle();
    key('v', Qt::ControlModifier);
    QCOMPARE(verbRuns().size(), 1);
    QCOMPARE(verbRuns().first(), (QStringList{"cp", "--", one, alpha}));
    QCOMPARE(tab()->cursorPath(), QDir(alpha).filePath(QStringLiteral("one.txt")));
    // A copy stays on the clipboard for the next paste.
    QVERIFY(m_clipboard->holdsFiles());

    // Cut takes the marked set when the row is in it, in listing order; the
    // status bar says what is waiting, and a paste is `mv`, after which the
    // clipboard is empty: those files are no longer where it said.
    tab()->setCwd(m_tree.path());
    settle();
    focusRow(QStringLiteral("one.txt"));
    key(Qt::Key_Space);
    focusRow(QStringLiteral("two.md"));
    key(Qt::Key_Space);
    key('x', Qt::ControlModifier);
    QCOMPARE(m_clipboard->files(), (QStringList{one, two}));
    QVERIFY(m_clipboard->cutting());
    data = QGuiApplication::clipboard()->mimeData();
    QVERIFY(data && data->hasFormat(QStringLiteral("application/x-kde-cutselection")));
    QObject *status = root->findChild<QObject *>(QStringLiteral("statusBar"));
    QVERIFY(status);
    QVERIFY2(status->property("spoken").toString().contains(QStringLiteral("2 to move")),
             qPrintable(status->property("spoken").toString()));
    key('A');

    verbRuns().clear();
    tab()->setCwd(alpha);
    settle();
    key('m');
    QVERIFY2(chooseInMenu(QStringLiteral("fileMenu"), QStringLiteral("Move 2 files here")),
             qPrintable(menuLabels(QStringLiteral("fileMenu")).join(QLatin1String(" | "))));
    QCOMPARE(verbRuns().size(), 2);
    QCOMPARE(verbRuns().at(0), (QStringList{"mv", "--", one, alpha}));
    QCOMPARE(verbRuns().at(1), (QStringList{"mv", "--", two, alpha}));
    QVERIFY(!m_clipboard->holdsFiles());
    key('m');
    QVERIFY(!menuLabels(QStringLiteral("fileMenu")).contains(QStringLiteral("Paste")));
    key(Qt::Key_Escape);

    // What another file manager put there pastes here too, and a tags tab has
    // nowhere to paste it.
    auto *foreign = new QMimeData;
    foreign->setUrls({QUrl::fromLocalFile(two)});
    QGuiApplication::clipboard()->setMimeData(foreign);
    settle();
    QVERIFY(m_clipboard->holdsFiles());
    m_workspace->addTab(QStringLiteral("tags"));
    settle();
    verbRuns().clear();
    key('v', Qt::ControlModifier);
    QVERIFY(verbRuns().isEmpty());
    m_workspace->closeCurrentTab();
    settle();
    key('v', Qt::ControlModifier);
    QCOMPARE(verbRuns().size(), 1);
    QCOMPARE(verbRuns().first(), (QStringList{"cp", "--", two, alpha}));

    m_clipboard->clear();
    tab()->setCwd(m_tree.path());
    settle();
}

// Three ways of marking more than one row at a time, each on top of the last.
void TestStudio::marksARunARangeAndAPattern()
{
    QObject *root = m_engine->rootObjects().first();
    QObject *folder = filesRoot()->property("folder").value<QObject *>();
    const auto marked = [this](const QString &name) {
        focusRow(name);
        return filesRoot()->property("entry").toMap()
            .value(QStringLiteral("marked")).toBool();
    };
    QMetaObject::invokeMethod(folder, "clearMarks");

    // `J` marks and steps down; `K` marks and steps up.
    focusRow(QStringLiteral("one.txt"));
    const int start = cursor();
    key('J');
    QCOMPARE(cursor(), start + 1);
    QCOMPARE(folder->property("markedCount").toInt(), 1);
    key('K');
    QCOMPARE(cursor(), start);
    QCOMPARE(folder->property("markedCount").toInt(), 2);
    QVERIFY(marked(QStringLiteral("one.txt")));
    key('A');
    QCOMPARE(folder->property("markedCount").toInt(), 0);

    // `*` asks for a pattern, and Enter marks what it names, case aside.
    // Counted off the rows rather than off the fixture, because cases before
    // this one leave files of their own in the tree.
    const auto rowsEndingIn = [&](const QString &suffix) {
        int n = 0;
        for (int i = 0; i < folder->property("count").toInt(); ++i) {
            QString path;
            QMetaObject::invokeMethod(folder, "pathAt", Q_RETURN_ARG(QString, path),
                                      Q_ARG(int, i));
            if (path.endsWith(suffix, Qt::CaseInsensitive))
                ++n;
        }
        return n;
    };
    const int texts = rowsEndingIn(QStringLiteral(".txt"));
    const int marks = rowsEndingIn(QStringLiteral(".md"));
    QVERIFY(texts >= 3 && marks >= 2);
    key('*');
    QCOMPARE(root->property("prompt").toString(), QStringLiteral("match"));
    QMetaObject::invokeMethod(root, "finishMarking", Q_ARG(QVariant, QVariant(true)),
                              Q_ARG(QVariant, QVariant(QStringLiteral("*.TXT"))));
    settle();
    QCOMPARE(root->property("prompt").toString(), QString());
    QCOMPARE(folder->property("markedCount").toInt(), texts);
    QVERIFY(marked(QStringLiteral("one.txt")));
    QVERIFY(marked(QStringLiteral("seven.txt")));
    QVERIFY(marked(QStringLiteral("plain.txt")));
    QVERIFY(!marked(QStringLiteral("two.md")));

    // On top of what was marked, not instead of it.
    key('*');
    QMetaObject::invokeMethod(root, "finishMarking", Q_ARG(QVariant, QVariant(true)),
                              Q_ARG(QVariant, QVariant(QStringLiteral("*.md"))));
    settle();
    QCOMPARE(folder->property("markedCount").toInt(), texts + marks);
    key('A');

    // Shift and a click marks from the cursor to the row clicked, both ends
    // in, whichever is above.
    const int from = rowOf(QStringLiteral("alpha"));
    const int to = rowOf(QStringLiteral("one.txt"));
    QVERIFY(from >= 0 && to > from);
    QMetaObject::invokeMethod(filesRoot(), "setCursor", Q_ARG(QVariant, QVariant(to)));
    settle();
    window()->grabWindow();
    QQuickItem *target = viewItemAt(filesRoot()->property("view").value<QObject *>(), from);
    QVERIFY2(target, "the row to click is not on screen");
    const QPointF at = target->mapToScene(QPointF(target->width() * 0.3,
                                                  target->height() * 0.5));
    QTest::mouseClick(window(), Qt::LeftButton, Qt::ShiftModifier, at.toPoint());
    settle();
    QCOMPARE(cursor(), from);
    QCOMPARE(folder->property("markedCount").toInt(), to - from + 1);
    QVERIFY(marked(QStringLiteral("alpha")));
    QVERIFY(marked(QStringLiteral("one.txt")));

    key('A');
    QCOMPARE(folder->property("markedCount").toInt(), 0);
}

// `f` is `ls --name <glob> --cached` as a listing: every name under the roots
// that fits, out of the lists `sweep` wrote, with their age said out loud.
void TestStudio::findsByNameAcrossTheRoots()
{
    QObject *root = m_engine->rootObjects().first();
    QTemporaryDir cache;
    QTemporaryDir shelf;
    QVERIFY(cache.isValid() && shelf.isValid());
    const QDir room(shelf.path());
    for (const char *name : {"paper.pdf", "SLIDES.PDF", "notes.txt"}) {
        QFile file(room.filePath(QString::fromLatin1(name)));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("x");
    }
    const QByteArray previousCache = qgetenv("XDG_CACHE_HOME");
    qputenv("XDG_CACHE_HOME", cache.path().toUtf8());
    const QVector<Root> previousRoots =
        declareRoots({Root{QStringLiteral("shelf"), room.absolutePath(), false, QString()}});
    const auto restore = qScopeGuard([&previousCache, &previousRoots] {
        qputenv("XDG_CACHE_HOME", previousCache);
        declareRoots(previousRoots);
    });
    const QDir names(QDir(cache.path()).filePath(QStringLiteral("omafiles/names")));
    QVERIFY(QDir().mkpath(names.absolutePath()));
    QFile list(names.filePath(QStringLiteral("shelf")));
    QVERIFY(list.open(QIODevice::WriteOnly | QIODevice::Text));
    list.write("SLIDES.PDF\nnotes.txt\npaper.pdf\n");
    list.close();

    QObject *folder = filesRoot()->property("folder").value<QObject *>();
    const auto files = [folder] {
        QStringList out;
        for (int i = 0; i < folder->property("count").toInt(); ++i) {
            QVariantMap entry;
            QMetaObject::invokeMethod(folder, "entryAt", Q_RETURN_ARG(QVariantMap, entry),
                                      Q_ARG(int, i));
            if (entry.value(QStringLiteral("kind")).toString() == QLatin1String("file"))
                out.append(entry.value(QStringLiteral("name")).toString());
        }
        return out;
    };

    // Asked from the prompt bar, folded, and titled with the pattern.
    key('f');
    QCOMPARE(root->property("prompt").toString(), QStringLiteral("find"));
    QMetaObject::invokeMethod(root, "finishFinding", Q_ARG(QVariant, QVariant(true)),
                              Q_ARG(QVariant, QVariant(QStringLiteral("*.pdf"))));
    settle();
    QCOMPARE(tab()->pattern(), QStringLiteral("*.pdf"));
    QCOMPARE(tab()->title(), QStringLiteral("*.pdf"));
    QCOMPARE(files(), (QStringList{QStringLiteral("paper.pdf"), QStringLiteral("SLIDES.PDF")}));
    QVERIFY2(folder->property("error").toString().contains(QStringLiteral("as of ")),
             qPrintable(folder->property("error").toString()));
    QObject *crumbs = root->findChild<QObject *>(QStringLiteral("pathCrumbs"));
    QVERIFY(crumbs);
    QCOMPARE(crumbs->property("full").toString(), QStringLiteral("*.pdf"));

    // A search is nowhere in particular, so nothing is made or pasted in it,
    // and the menu says how to leave it.
    key('n');
    QCOMPARE(root->property("prompt").toString(), QString());
    key('m');
    QVERIFY(menuLabels(QStringLiteral("fileMenu")).contains(QStringLiteral("Leave the search")));
    key(Qt::Key_Escape);

    // `h` is the way back to the folder the tab was standing in.
    key('h');
    QCOMPARE(tab()->pattern(), QString());
    QCOMPARE(cwd(), m_tree.path());
    QVERIFY(files().contains(QStringLiteral("one.txt")));
}

// `f` and `*` ask fnmatch(3), the question `ls --name` asks: a backslash
// escapes, and a lone `[` is itself rather than a pattern that matches nothing.
// A gap line in the list is not a name.
void TestStudio::matchesAPatternTheWayTheCliDoes()
{
    QObject *root = m_engine->rootObjects().first();
    QTemporaryDir cache;
    QTemporaryDir shelf;
    QVERIFY(cache.isValid() && shelf.isValid());
    const QDir room(shelf.path());
    for (const char *name : {"[", "a*b", "axb"}) {
        QFile file(room.filePath(QString::fromLatin1(name)));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("x");
    }
    const QByteArray previousCache = qgetenv("XDG_CACHE_HOME");
    qputenv("XDG_CACHE_HOME", cache.path().toUtf8());
    const QVector<Root> previousRoots =
        declareRoots({Root{QStringLiteral("patterns"), room.absolutePath(), false, QString()}});
    const auto restore = qScopeGuard([&previousCache, &previousRoots] {
        qputenv("XDG_CACHE_HOME", previousCache);
        declareRoots(previousRoots);
    });
    QVERIFY(writeNames(QStringLiteral("patterns"),
                       {QStringLiteral("["), QStringLiteral("a*b"), QStringLiteral("axb")},
                       {{room.filePath(QStringLiteral("a[b")),
                         QStringLiteral("a[b is unreadable; nothing under it was listed")}},
                       nullptr, nullptr));

    QObject *folder = filesRoot()->property("folder").value<QObject *>();
    const auto files = [folder] {
        QStringList out;
        for (int i = 0; i < folder->property("count").toInt(); ++i) {
            QVariantMap entry;
            QMetaObject::invokeMethod(folder, "entryAt", Q_RETURN_ARG(QVariantMap, entry),
                                      Q_ARG(int, i));
            if (entry.value(QStringLiteral("kind")).toString() == QLatin1String("file"))
                out.append(entry.value(QStringLiteral("name")).toString());
        }
        return out;
    };
    const auto find = [&](const QString &pattern) {
        key('f');
        QMetaObject::invokeMethod(root, "finishFinding", Q_ARG(QVariant, QVariant(true)),
                                  Q_ARG(QVariant, QVariant(pattern)));
        settle();
        return files();
    };

    QCOMPARE(find(QStringLiteral("[")), QStringList{QStringLiteral("[")});
    QCOMPARE(find(QStringLiteral("a\\*b")), QStringList{QStringLiteral("a*b")});
    QCOMPARE(find(QStringLiteral("*")),
             (QStringList{QStringLiteral("["), QStringLiteral("a*b"), QStringLiteral("axb")}));
    // Said beside the age, the way `ls --cached` says it.
    QVERIFY2(folder->property("error").toString().contains(
                 QStringLiteral("the list is short: a[b is unreadable")),
             qPrintable(folder->property("error").toString()));

    // And `*` over those rows marks by the same rule.
    int matched = -1;
    QMetaObject::invokeMethod(folder, "markMatching", Q_RETURN_ARG(int, matched),
                              Q_ARG(QString, QStringLiteral("a\\*b")));
    QCOMPARE(matched, 1);
    QMetaObject::invokeMethod(folder, "markMatching", Q_RETURN_ARG(int, matched),
                              Q_ARG(QString, QStringLiteral("[")));
    QCOMPARE(matched, 1);
    key('A');
    key('h');
    QCOMPARE(tab()->pattern(), QString());
}

// A script in the config folder is a menu entry, run over the marked set with
// the paths as its arguments and their folder as its working directory --
// through the launcher seam, so nothing here runs it.
void TestStudio::runsAScriptOverTheMarkedSet()
{
    const QDir tree(m_tree.path());
    const QDir scripts(QDir(m_desktop.path()).filePath(QStringLiteral("config/omafiles/scripts")));
    QVERIFY(QDir().mkpath(scripts.absolutePath()));
    const QString script = scripts.filePath(QStringLiteral("count"));
    QFile file(script);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("#!/bin/sh\necho $#\n");
    file.close();
    // Not executable yet, so not a script yet: a text nobody chmod'd is one
    // that would only ever fail.
    QVERIFY(file.setPermissions(QFile::ReadOwner | QFile::WriteOwner));
    m_index->reload();
    settle();
    settle();
    QVERIFY(!m_index->hasScripts());
    focusRow(QStringLiteral("one.txt"));
    key('m');
    QVERIFY(!menuLabels(QStringLiteral("fileMenu")).contains(QStringLiteral("Run a script over it…")));
    key(Qt::Key_Escape);

    QVERIFY(file.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
    m_index->reload();
    settle();
    settle();
    QVERIFY(m_index->hasScripts());

    focusRow(QStringLiteral("one.txt"));
    key(Qt::Key_Space);
    focusRow(QStringLiteral("two.md"));
    key(Qt::Key_Space);
    openRequests().clear();
    key('m');
    QVERIFY2(chooseInMenu(QStringLiteral("fileMenu"), QStringLiteral("Run a script over 2 files…")),
             qPrintable(menuLabels(QStringLiteral("fileMenu")).join(QLatin1String(" | "))));
    settle();
    QVERIFY2(chooseInMenu(QStringLiteral("scriptsMenu"), QStringLiteral("count")),
             qPrintable(menuLabels(QStringLiteral("scriptsMenu")).join(QLatin1String(" | "))));
    QCOMPARE(openRequests().size(), 1);
    const OpenRequest &ran = openRequests().first();
    QCOMPARE(ran.kind, OpenRequest::Script);
    QCOMPARE(ran.application, script);
    QCOMPARE(ran.arguments, (QStringList{tree.filePath(QStringLiteral("one.txt")),
                                         tree.filePath(QStringLiteral("two.md"))}));
    QCOMPARE(ran.path, m_tree.path());
    QVERIFY2(m_index->notice().contains(QStringLiteral("ran count over 2 files")),
             qPrintable(m_index->notice()));
    key('A');

    QVERIFY(QFile::remove(script));
    m_index->reload();
    settle();
    settle();
}

// A link beside the row, in the desktop's spelling, and the clipboard's files
// as links in the folder on screen. Both refuse a name that is taken.
void TestStudio::makesLinksBesideAndHere()
{
    const QDir tree(m_tree.path());
    const QString one = tree.filePath(QStringLiteral("one.txt"));
    const QString beside = tree.filePath(QStringLiteral("Link to one.txt"));

    focusRow(QStringLiteral("one.txt"));
    key('m');
    QVERIFY(chooseInMenu(QStringLiteral("fileMenu"), QStringLiteral("Make a link beside")));
    QVERIFY(QFileInfo(beside).isSymLink());
    QCOMPARE(QFileInfo(beside).symLinkTarget(), one);
    QVERIFY2(m_index->notice().startsWith(QStringLiteral("linked ")), qPrintable(m_index->notice()));
    // Listed under its own name. A row is filed by what it resolves to, so
    // the link and its target share a path and the name is what tells them
    // apart.
    QObject *folder = filesRoot()->property("folder").value<QObject *>();
    QStringList names;
    for (int i = 0; i < folder->property("count").toInt(); ++i) {
        QVariantMap entry;
        QMetaObject::invokeMethod(folder, "entryAt", Q_RETURN_ARG(QVariantMap, entry),
                                  Q_ARG(int, i));
        names.append(entry.value(QStringLiteral("name")).toString());
    }
    QVERIFY2(names.contains(QStringLiteral("Link to one.txt")),
             qPrintable(names.join(QLatin1String(" | "))));
    // A second link beside the same file is a name already taken, refused in
    // `mv`'s words.
    focusRow(QStringLiteral("one.txt"));
    key('m');
    QVERIFY(chooseInMenu(QStringLiteral("fileMenu"), QStringLiteral("Make a link beside")));
    QVERIFY2(m_index->notice().contains(QStringLiteral("already exists")),
             qPrintable(m_index->notice()));

    // The clipboard's files, linked into another folder under their names.
    focusRow(QStringLiteral("one.txt"));
    key('c', Qt::ControlModifier);
    const QString alpha = tree.filePath(QStringLiteral("alpha"));
    tab()->setCwd(alpha);
    settle();
    key('m');
    QVERIFY2(chooseInMenu(QStringLiteral("fileMenu"), QStringLiteral("Link it here")),
             qPrintable(menuLabels(QStringLiteral("fileMenu")).join(QLatin1String(" | "))));
    const QString here = QDir(alpha).filePath(QStringLiteral("one.txt"));
    QTRY_VERIFY(QFileInfo(here).isSymLink());
    QCOMPARE(QFileInfo(here).symLinkTarget(), one);
    QVERIFY2(m_clipboard->holdsFiles(), "linking emptied the clipboard");

    m_clipboard->clear();
    QVERIFY(QFile::remove(here));
    QVERIFY(QFile::remove(beside));
    tab()->setCwd(m_tree.path());
    settle();
}

// The desktop's recent list as a listing, in the desktop's order, with `h`
// the way back to the folder the tab was standing in.
void TestStudio::listsWhatTheDesktopOpenedLately()
{
    QObject *root = m_engine->rootObjects().first();
    const QDir tree(m_tree.path());
    QFile xbel(QDir(m_home.path()).filePath(QStringLiteral("recently-used.xbel")));
    QVERIFY(xbel.open(QIODevice::WriteOnly | QIODevice::Text));
    xbel.write(QStringLiteral(
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<xbel version=\"1.0\">\n"
        "  <bookmark href=\"file://%1\" visited=\"2026-09-01T10:00:00Z\"/>\n"
        "  <bookmark href=\"file://%2\" visited=\"2026-09-02T10:00:00Z\"/>\n"
        "  <bookmark href=\"file://%3\" visited=\"2026-09-03T10:00:00Z\"/>\n"
        "</xbel>\n")
        .arg(tree.filePath(QStringLiteral("one.txt")),
             tree.filePath(QStringLiteral("alpha/inner/absent.txt")),
             tree.filePath(QStringLiteral("two.md")))
        .toUtf8());
    xbel.close();

    QObject *folder = filesRoot()->property("folder").value<QObject *>();
    const auto names = [folder] {
        QStringList out;
        for (int i = 0; i < folder->property("count").toInt(); ++i) {
            QVariantMap entry;
            QMetaObject::invokeMethod(folder, "entryAt", Q_RETURN_ARG(QVariantMap, entry),
                                      Q_ARG(int, i));
            if (entry.value(QStringLiteral("kind")).toString() == QLatin1String("file"))
                out.append(entry.value(QStringLiteral("name")).toString());
        }
        return out;
    };

    QMetaObject::invokeMethod(root, "openRecent");
    settle();
    QVERIFY(tab()->recent());
    QCOMPARE(tab()->title(), QStringLiteral("(recent)"));
    // Newest first, and only what is there.
    QCOMPARE(names(), (QStringList{QStringLiteral("two.md"), QStringLiteral("one.txt")}));
    QObject *crumbs = root->findChild<QObject *>(QStringLiteral("pathCrumbs"));
    QVERIFY(crumbs);
    QCOMPARE(crumbs->property("full").toString(), QStringLiteral("(recent)"));

    // Nowhere in particular, so nothing is made in it; the menu says how out.
    key('n');
    QCOMPARE(root->property("prompt").toString(), QString());
    key('m');
    QVERIFY(menuLabels(QStringLiteral("fileMenu")).contains(QStringLiteral("Leave the recent list")));
    key(Qt::Key_Escape);
    key('h');
    QVERIFY(!tab()->recent());
    QCOMPARE(cwd(), m_tree.path());

    QVERIFY(xbel.remove());
}

// The details columns are one list: the headings and every row's cells follow
// it, the heading menu and the palette both change it, and a fresh workspace
// reads it back from the state directory.
void TestStudio::choosesTheDetailsColumnsAndKeepsThem()
{
    const QStringList defaults{QStringLiteral("tags"), QStringLiteral("type"),
                               QStringLiteral("size"), QStringLiteral("mtime")};
    QCOMPARE(m_workspace->columns(), defaults);
    const auto restore = qScopeGuard([this, &defaults] {
        for (const QString &field : m_workspace->columns())
            m_workspace->toggleColumn(field);
        for (const QString &field : defaults)
            m_workspace->toggleColumn(field);
    });
    tab()->setViewMode(QStringLiteral("details"));
    settle();

    const auto byX = [](QList<QQuickItem *> items) {
        std::sort(items.begin(), items.end(), [](QQuickItem *a, QQuickItem *b) {
            return a->mapToScene({0, 0}).x() < b->mapToScene({0, 0}).x();
        });
        return items;
    };
    const auto heading = [this](const QString &label) -> QQuickItem * {
        for (QQuickItem *item : itemsNamed(QStringLiteral("columnHead")))
            if (item->isVisible() && item->property("label").toString() == label)
                return item;
        return nullptr;
    };
    const auto headings = [&] {
        QList<QQuickItem *> shown;
        for (QQuickItem *item : itemsNamed(QStringLiteral("columnHead")))
            if (item->isVisible())
                shown.append(item);
        QStringList labels;
        for (QQuickItem *item : byX(shown))
            labels.append(item->property("label").toString());
        return labels;
    };
    // True when every drawn row has exactly these cells, left to right.
    const auto everyRowHas = [&](const QStringList &fields) {
        QHash<QQuickItem *, QList<QQuickItem *>> rows;
        for (QQuickItem *item : itemsNamed(QStringLiteral("columnCell")))
            rows[item->parentItem()].append(item);
        if (rows.isEmpty())
            return false;
        for (const QList<QQuickItem *> &cells : std::as_const(rows)) {
            QStringList drawn;
            for (QQuickItem *cell : byX(cells))
                drawn.append(cell->property("modelData").toString());
            if (drawn != fields)
                return false;
        }
        return true;
    };
    const auto cellText = [&](const QString &row, const QString &field) {
        for (QQuickItem *cell : itemsNamed(QStringLiteral("columnCell"))) {
            if (cell->property("modelData").toString() != field
                || cell->parentItem()->parentItem()->property("name").toString() != row)
                continue;
            QObject *drawn = cell->property("item").value<QObject *>();
            return drawn ? drawn->property("text").toString() : QString();
        }
        return QStringLiteral("<no cell>");
    };
    const auto click = [this](QQuickItem *item, Qt::MouseButton button) {
        QTest::mouseClick(window(), button, Qt::NoModifier,
                          item->mapToScene({item->width() / 2, item->height() / 2}).toPoint());
        settle();
    };
    const auto palette = [this](const QByteArray &query) {
        key(':');
        for (char ch : query)
            key(ch);
        key(Qt::Key_Return);
    };
    // A Row places its children at the next polish, so until then every
    // heading and cell sits at the same x and no order can be read off them.
    const auto laidOut = [&] {
        QHash<QQuickItem *, QSet<qreal>> xs;
        QHash<QQuickItem *, int> counts;
        for (const QString &name : {QStringLiteral("columnHead"), QStringLiteral("columnCell")}) {
            for (QQuickItem *item : itemsNamed(name)) {
                if (!item->isVisible())
                    continue;
                xs[item->parentItem()].insert(item->mapToScene({0, 0}).x());
                ++counts[item->parentItem()];
            }
        }
        for (auto it = counts.cbegin(); it != counts.cend(); ++it)
            if (xs.value(it.key()).size() != it.value())
                return false;
        return true;
    };

    QTRY_VERIFY(laidOut());
    QCOMPARE(headings(), (QStringList{"name", "tags", "type", "size", "modified"}));
    QVERIFY(everyRowHas(defaults));

    // A right-click on one heading offers to move that one, and to hide any.
    QQuickItem *sizeHead = heading(QStringLiteral("size"));
    QVERIFY(sizeHead);
    click(sizeHead, Qt::RightButton);
    const QStringList offered = menuLabels(QStringLiteral("columnMenu"));
    QVERIFY2(offered.contains(QStringLiteral("Move the size column left")),
             qPrintable(offered.join(QLatin1Char('|'))));
    QVERIFY(offered.contains(QStringLiteral("Move the size column right")));
    QVERIFY(!offered.contains(QStringLiteral("Move the type column left")));
    QVERIFY(offered.contains(QStringLiteral("Show the owner column")));
    QVERIFY(chooseInMenu(QStringLiteral("columnMenu"), QStringLiteral("Hide the size column")));
    const QStringList hidden{QStringLiteral("tags"), QStringLiteral("type"),
                             QStringLiteral("mtime")};
    QCOMPARE(m_workspace->columns(), hidden);
    QTRY_VERIFY(laidOut());
    QCOMPARE(headings(), (QStringList{"name", "tags", "type", "modified"}));
    QVERIFY(everyRowHas(hidden));

    // Over the name, which never moves, it offers to move every column.
    click(heading(QStringLiteral("name")), Qt::RightButton);
    const QStringList overName = menuLabels(QStringLiteral("columnMenu"));
    QVERIFY2(overName.contains(QStringLiteral("Move the tags column right"))
                 && overName.contains(QStringLiteral("Move the modified column left")),
             qPrintable(overName.join(QLatin1Char('|'))));
    key(Qt::Key_Escape);

    // The palette reaches the same commands from the keyboard.
    palette("show the owner");
    palette("show the mode");
    QCOMPARE(m_workspace->columns(),
             (QStringList{"tags", "type", "mtime", "owner", "mode"}));
    palette("move the owner column left");
    const QStringList moved{QStringLiteral("tags"), QStringLiteral("type"),
                            QStringLiteral("owner"), QStringLiteral("mtime"),
                            QStringLiteral("mode")};
    QCOMPARE(m_workspace->columns(), moved);
    QTRY_VERIFY(laidOut());
    QCOMPARE(headings(), (QStringList{"name", "tags", "type", "owner", "modified", "mode"}));
    QVERIFY(everyRowHas(moved));

    // The new cells say what the details panel says about the same row.
    focusRow(QStringLiteral("two.md"));
    const QVariantMap entry = filesRoot()->property("entry").toMap();
    const QString mode = entry.value(QStringLiteral("mode")).toString();
    QVERIFY2(mode.startsWith(QStringLiteral("-rw")), qPrintable(mode));
    QCOMPARE(cellText(QStringLiteral("two.md"), QStringLiteral("mode")), mode);
    const passwd *me = getpwuid(getuid());
    QVERIFY(me);
    QCOMPARE(cellText(QStringLiteral("two.md"), QStringLiteral("owner")),
             QString::fromLocal8Bit(me->pw_name));

    // A heading still sorts wherever it now stands; one the tab cannot sort by
    // does nothing.
    QQuickItem *ownerHead = heading(QStringLiteral("owner"));
    QVERIFY(ownerHead);
    click(ownerHead, Qt::LeftButton);
    QCOMPARE(tab()->sortRole(), QStringLiteral("name"));
    QVERIFY(tab()->sortAscending());
    QQuickItem *typeHead = heading(QStringLiteral("type"));
    QVERIFY(typeHead);
    click(typeHead, Qt::LeftButton);
    QCOMPARE(tab()->sortRole(), QStringLiteral("type"));

    // What the next window starts with.
    QVERIFY(QFileInfo::exists(
        QDir(m_state.path()).filePath(QStringLiteral("omafiles/studio.conf"))));
    Workspace next;
    QCOMPARE(next.columns(), moved);

    // Every column hidden is a choice too, and not the absence of one.
    for (const QString &field : moved)
        m_workspace->toggleColumn(field);
    QVERIFY(Workspace().columns().isEmpty());

    // A hand-edited file keeps the fields it names once and drops the rest.
    {
        QSettings state(QDir(m_state.path()).filePath(QStringLiteral("omafiles/studio.conf")),
                        QSettings::IniFormat);
        state.setValue(QStringLiteral("details/columns"),
                       QStringList{"mode", "colour", "mode", "size"});
    }
    QCOMPARE(Workspace().columns(), (QStringList{"mode", "size"}));
}

// One left-button event with the modifiers held, the way a hand holds them
// through a drag. QTest's own mouseMove sends none.
static void sendLeft(QQuickWindow *window, QEvent::Type type, const QPointF &at,
                     Qt::KeyboardModifiers mods = Qt::NoModifier)
{
    const Qt::MouseButton button = type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton;
    const Qt::MouseButtons held = type == QEvent::MouseButtonRelease ? Qt::NoButton
                                                                     : Qt::LeftButton;
    QMouseEvent event(type, at, window->mapToGlobal(at), button, held, mods);
    QCoreApplication::sendEvent(window, &event);
}

// Presses at `from` and moves to `to` in steps, past the drag threshold,
// without letting go.
static void pressAndDrag(QQuickWindow *window, const QPointF &from, const QPointF &to,
                         Qt::KeyboardModifiers mods = Qt::NoModifier)
{
    sendLeft(window, QEvent::MouseButtonPress, from, mods);
    for (int step = 1; step <= 8; ++step)
        sendLeft(window, QEvent::MouseMove, from + (to - from) * step / 8.0, mods);
}

// Dragging from where no row claims the press draws a band and marks what it
// crosses: plain replaces, Shift adds, Ctrl toggles, Escape puts the marks
// back, and the view scrolls when the band reaches its edge.
void TestStudio::marksWhatARubberBandCrosses()
{
    QObject *root = m_engine->rootObjects().first();
    QObject *folder = filesRoot()->property("folder").value<QObject *>();
    QQuickItem *view = qobject_cast<QQuickItem *>(filesRoot()->property("view").value<QObject *>());
    QObject *band = filesRoot()->property("band").value<QObject *>();
    QVERIFY(folder && view && band);
    // A failed check returns with the button down; the next case gets it up.
    const auto restore = qScopeGuard([&] {
        sendLeft(window(), QEvent::MouseButtonRelease, QPointF(1, 1));
        QMetaObject::invokeMethod(folder, "clearMarks");
        tab()->setViewMode(QStringLiteral("list"));
        tab()->setCwd(m_tree.path());
        root->setProperty("filtering", false);
        QMetaObject::invokeMethod(root, "takeFocus");
        settle();
    });
    QMetaObject::invokeMethod(folder, "clearMarks");
    settle();
    window()->grabWindow();

    const auto markedRows = [&] {
        QList<int> rows;
        for (int i = 0; i < folder->property("count").toInt(); ++i) {
            QString path;
            QMetaObject::invokeMethod(folder, "pathAt", Q_RETURN_ARG(QString, path),
                                      Q_ARG(int, i));
            bool marked = false;
            QMetaObject::invokeMethod(folder, "isMarked", Q_RETURN_ARG(bool, marked),
                                      Q_ARG(QString, path));
            if (marked)
                rows.append(i);
        }
        return rows;
    };
    const auto rowsFrom = [](int first, int last) {
        QList<int> rows;
        for (int i = first; i <= last; ++i)
            rows.append(i);
        return rows;
    };
    // Past the name, where the list leaves the press to the band.
    const auto beside = [&](int row) {
        QQuickItem *item = viewItemAt(view, row);
        return item ? item->mapToScene(QPointF(item->width() * 0.55, item->height() * 0.5))
                    : QPointF();
    };
    const auto release = [&](const QPointF &at) {
        sendLeft(window(), QEvent::MouseButtonRelease, at);
        settle();
    };

    const int a = rowOf(QStringLiteral("beta"));
    QVERIFY(a >= 0);
    const int b = a + 2;
    const int c = b + 2;
    QVERIFY(c < folder->property("count").toInt());
    for (int row = 1; row <= c; ++row) {
        QString kind;
        QMetaObject::invokeMethod(folder, "kindAt", Q_RETURN_ARG(QString, kind), Q_ARG(int, row));
        QVERIFY(kind != QLatin1String("parent"));
    }

    // A plain drag marks what it covers from the move that starts it, and
    // gives rows back as it shrinks. Like a click on a row, it takes the
    // keyboard back to the list.
    key('/');
    QCOMPARE(root->property("focusRegion").toString(), QStringLiteral("filter"));
    QMetaObject::invokeMethod(folder, "toggleMarkAt", Q_ARG(int, c));
    const int cursorBefore = cursor();
    sendLeft(window(), QEvent::MouseButtonPress, beside(a));
    sendLeft(window(), QEvent::MouseMove, beside(b));
    QVERIFY(band->property("live").toBool());
    QCOMPARE(markedRows(), rowsFrom(a, b));
    QCOMPARE(root->property("focusRegion").toString(), QStringLiteral("list"));
    QVERIFY(!root->property("filtering").toBool());
    sendLeft(window(), QEvent::MouseMove, beside(a));
    QCOMPARE(markedRows(), QList<int>{a});
    sendLeft(window(), QEvent::MouseMove, beside(b));
    release(beside(b));
    QVERIFY(!band->property("live").toBool());
    QCOMPARE(markedRows(), rowsFrom(a, b));
    QCOMPARE(cursor(), cursorBefore);

    // Shift adds to what was marked.
    QMetaObject::invokeMethod(folder, "clearMarks");
    QMetaObject::invokeMethod(folder, "toggleMarkAt", Q_ARG(int, c));
    pressAndDrag(window(), beside(a), beside(b), Qt::ShiftModifier);
    release(beside(b));
    QCOMPARE(markedRows(), rowsFrom(a, b) << c);

    // Ctrl turns each row it crosses over, and leaves the rest alone.
    pressAndDrag(window(), beside(a), beside(a + 1), Qt::ControlModifier);
    release(beside(a + 1));
    QCOMPARE(markedRows(), (QList<int>{b, c}));

    // Escape during the drag puts the marks back and ends the band.
    pressAndDrag(window(), beside(a), beside(b));
    QCOMPARE(markedRows(), rowsFrom(a, b));
    QTest::keyClick(window(), Qt::Key_Escape);
    settle();
    QVERIFY(!band->property("live").toBool());
    QCOMPARE(markedRows(), (QList<int>{b, c}));
    sendLeft(window(), QEvent::MouseMove, beside(c));
    release(beside(c));
    QCOMPARE(markedRows(), (QList<int>{b, c}));

    // The icon and the name are the row's: a drag from there draws nothing.
    QQuickItem *first = viewItemAt(view, a);
    QVERIFY(first);
    const QPointF onName = first->mapToScene(
        QPointF(filesRoot()->property("nameIndent").toReal() + 4, first->height() * 0.5));
    pressAndDrag(window(), onName, beside(b));
    QVERIFY(!band->property("live").toBool());
    release(beside(b));
    QCOMPARE(markedRows(), (QList<int>{b, c}));

    // A click beside the name is still a click on the row, and Shift and a
    // click still marks a range on top of what is marked.
    QTest::mouseClick(window(), Qt::LeftButton, Qt::NoModifier, beside(a).toPoint());
    settle();
    QCOMPARE(cursor(), a);
    QCOMPARE(markedRows(), (QList<int>{b, c}));
    QTest::mouseClick(window(), Qt::LeftButton, Qt::ShiftModifier, beside(a + 1).toPoint());
    settle();
    QCOMPARE(markedRows(), (QList<int>{a, a + 1, b, c}));

    // The details table puts its heading above the rows; the band still lands
    // on the rows it crosses, and a drag from the heading is not a band.
    QMetaObject::invokeMethod(folder, "clearMarks");
    tab()->setViewMode(QStringLiteral("details"));
    settle();
    window()->grabWindow();
    pressAndDrag(window(), beside(a), beside(b));
    release(beside(b));
    QCOMPARE(markedRows(), rowsFrom(a, b));
    QMetaObject::invokeMethod(folder, "clearMarks");
    const QPointF heading = view->mapToScene(QPointF(view->width() * 0.55, 8));
    pressAndDrag(window(), heading, beside(b));
    QVERIFY(!band->property("live").toBool());
    release(beside(b));
    QCOMPARE(markedRows(), QList<int>{});

    // In the grid the band crosses tiles side by side, from the space under a
    // tile's name.
    tab()->setViewMode(QStringLiteral("icons"));
    settle();
    QQuickItem *grid = qobject_cast<QQuickItem *>(filesRoot()->property("view").value<QObject *>());
    QVERIFY(grid && grid != view);
    window()->grabWindow();
    const auto under = [&](int cell) {
        QQuickItem *item = viewItemAt(grid, cell);
        return item ? item->mapToScene(QPointF(item->width() * 0.5, item->height() - 6))
                    : QPointF();
    };
    const qreal cellWidth = grid->property("cellWidth").toReal();
    // Row 0 is `..`, which no band marks; the three after it share a line.
    const int start = 1;
    QVERIFY(int(grid->width() / cellWidth) >= start + 3);
    pressAndDrag(window(), under(start), under(start + 2));
    release(under(start + 2));
    QCOMPARE(markedRows(), rowsFrom(start, start + 2));
    QQuickItem *tile = viewItemAt(grid, start + 1);
    QVERIFY(tile);
    QMetaObject::invokeMethod(folder, "clearMarks");
    pressAndDrag(window(), tile->mapToScene(QPointF(tile->width() * 0.5, 24)), under(start + 2));
    QVERIFY(!band->property("live").toBool());
    release(under(start + 2));
    QCOMPARE(markedRows(), QList<int>{});
    tab()->setViewMode(QStringLiteral("list"));
    settle();

    // Held against the bottom edge, the band scrolls the list and keeps
    // marking what comes into view.
    QTemporaryDir many;
    QVERIFY(many.isValid());
    for (int i = 0; i < 80; ++i) {
        QFile file(QDir(many.path()).filePath(
            QStringLiteral("f%1.txt").arg(i, 2, 10, QLatin1Char('0'))));
        QVERIFY(file.open(QIODevice::WriteOnly));
    }
    tab()->setCwd(many.path());
    settle();
    window()->grabWindow();
    QCOMPARE(markedRows(), QList<int>{});
    const qreal top = view->property("contentY").toReal();
    const QPointF edge = view->mapToScene(QPointF(view->width() * 0.55, view->height() - 2));
    pressAndDrag(window(), beside(1), edge);
    const int shown = markedRows().size();
    QTRY_VERIFY(view->property("contentY").toReal() > top + 60);
    release(edge);
    const QList<int> swept = markedRows();
    QVERIFY(swept.size() > shown + 1);
    QCOMPARE(swept, rowsFrom(1, swept.last()));
}

// The first visible item named `name` at or under `from`.
static QQuickItem *shownItem(QQuickItem *from, const QString &name)
{
    QList<QQuickItem *> queue{from};
    while (!queue.isEmpty()) {
        QQuickItem *item = queue.takeFirst();
        if (item->objectName() == name && item->isVisible())
            return item;
        queue.append(item->childItems());
    }
    return nullptr;
}

// A click on a mode bit in the details panel is `chmod` on the row, and the
// row menu runs it over the marked set only once the sheet has shown the mode
// and Enter has said yes. Nothing is changed here: the recorder answers.
void TestStudio::changesPermissionsThroughTheCommandLine()
{
    QObject *root = m_engine->rootObjects().first();
    const QDir tree(m_tree.path());
    const QString one = tree.filePath(QStringLiteral("one.txt"));
    const QString two = tree.filePath(QStringLiteral("two.md"));
    QVERIFY(::chmod(QFile::encodeName(one).constData(), 0640) == 0);
    QVERIFY(::chmod(QFile::encodeName(two).constData(), 0640) == 0);
    const auto restore = qScopeGuard([&] {
        ::chmod(QFile::encodeName(one).constData(), 0644);
        ::chmod(QFile::encodeName(two).constData(), 0644);
    });

    focusRow(QStringLiteral("one.txt"));
    QCOMPARE(filesRoot()->property("entry").toMap()
                 .value(QStringLiteral("permissions")).toInt(), 0640);

    const auto clickBit = [this](int cell) {
        // A frame lays the details panel out for the row just focused; before
        // one, a bit's position is the last row's and the click lands on the
        // views list beneath it.
        window()->grabWindow();
        QQuickItem *bits = shownItem(window()->contentItem(), QStringLiteral("detailsMode"));
        QQuickItem *bit = bits ? shownItem(bits, QStringLiteral("modeBit%1").arg(cell)) : nullptr;
        if (!bit)
            return false;
        const QPointF centre = bit->mapToScene(QPointF(bit->width() / 2, bit->height() / 2));
        QTest::mouseClick(window(), Qt::LeftButton, Qt::NoModifier, centre.toPoint());
        settle();
        return true;
    };

    // The owner's execute bit: 640 becomes 740, as the octal the verb takes.
    verbRuns().clear();
    QVERIFY2(clickBit(2), "the details panel shows no mode bits");
    QCOMPARE(verbRuns().size(), 1);
    QCOMPARE(verbRuns().first(), (QStringList{"chmod", "740", "--", one}));
    QVERIFY2(m_index->notice().contains(QStringLiteral("set 740 on 1 file")),
             qPrintable(m_index->notice()));

    // A link, and somebody else's file: read out, and a click runs nothing.
    QTemporaryDir other;
    QVERIFY(other.isValid());
    const QString mine = QDir(other.path()).filePath(QStringLiteral("a-target.txt"));
    QFile made(mine);
    QVERIFY(made.open(QIODevice::WriteOnly));
    made.close();
    QVERIFY(QFile::link(mine, QDir(other.path()).filePath(QStringLiteral("link"))));
    tab()->setCwd(other.path());
    settle();
    focusRow(QStringLiteral("link"));
    QCOMPARE(filesRoot()->property("entry").toMap().value(QStringLiteral("link")).toBool(), true);
    verbRuns().clear();
    QVERIFY(clickBit(0));
    QVERIFY2(verbRuns().isEmpty(), "a click on a link's mode ran chmod");
    key(Qt::Key_Escape);
    key('m');
    QVERIFY(chooseInMenu(QStringLiteral("fileMenu"), QStringLiteral("Permissions…")));
    settle();
    QCOMPARE(root->property("blocked").toBool(), true);
    key(Qt::Key_Space);
    key(Qt::Key_Return);
    QVERIFY2(verbRuns().isEmpty(), "the sheet ran chmod through a link");
    key(Qt::Key_Escape);
    if (::geteuid() != 0) {
        tab()->setCwd(QStringLiteral("/"));
        settle();
        focusRow(QStringLiteral("usr"));
        QCOMPARE(filesRoot()->property("entry").toMap().value(QStringLiteral("mine")).toBool(),
                 false);
        // The owner's bits, not what they come to for whoever is looking.
        struct stat usr {};
        QVERIFY(::stat("/usr", &usr) == 0);
        QCOMPARE(filesRoot()->property("entry").toMap()
                     .value(QStringLiteral("permissions")).toInt(), int(usr.st_mode & 0777));
        QVERIFY(clickBit(0));
        QVERIFY2(verbRuns().isEmpty(), "a click on somebody else's mode ran chmod");
        key(Qt::Key_Escape);
        key('m');
        QVERIFY(chooseInMenu(QStringLiteral("fileMenu"), QStringLiteral("Permissions…")));
        settle();
        QCOMPARE(root->property("blocked").toBool(), true);
        key(Qt::Key_Space);
        key(Qt::Key_Return);
        QVERIFY2(verbRuns().isEmpty(), "the sheet ran chmod over somebody else's file");
        key(Qt::Key_Escape);
    }
    tab()->setCwd(m_tree.path());
    settle();

    // Over a marked set: the sheet starts from the row's bits, the keys walk
    // and flip them, and Apply saves the draft.
    focusRow(QStringLiteral("one.txt"));
    key(Qt::Key_Space);
    focusRow(QStringLiteral("two.md"));
    key(Qt::Key_Space);
    focusRow(QStringLiteral("one.txt"));
    verbRuns().clear();
    key('m');
    QVERIFY2(chooseInMenu(QStringLiteral("fileMenu"), QStringLiteral("Permissions of 2 files…")),
             qPrintable(menuLabels(QStringLiteral("fileMenu")).join(QLatin1String(" | "))));
    settle();
    QCOMPARE(root->property("blocked").toBool(), true);
    QQuickItem *sheetBits = shownItem(window()->contentItem(), QStringLiteral("sheetMode"));
    QVERIFY(sheetBits);
    QCOMPARE(sheetBits->property("octal").toString(), QStringLiteral("640"));
    key('l');
    key('l');
    key(Qt::Key_Space);
    for (int i = 0; i < 3; ++i)
        key('l');
    key(Qt::Key_Space);
    QCOMPARE(sheetBits->property("octal").toString(), QStringLiteral("750"));
    QVERIFY2(verbRuns().isEmpty(), "the sheet ran chmod before it was told to");
    focusPermissionApply();
    key(Qt::Key_Return);
    QCOMPARE(verbRuns().size(), 1);
    QCOMPARE(verbRuns().first(), (QStringList{"chmod", "750", "--", one, two}));
    QCOMPARE(root->property("blocked").toBool(), false);

    // From the palette, over the row under the cursor; Esc runs nothing and
    // gives the keyboard back.
    verbRuns().clear();
    key(':');
    for (char ch : QByteArray("permissions"))
        key(ch);
    key(Qt::Key_Return);
    settle();
    QCOMPARE(root->property("blocked").toBool(), true);
    key(Qt::Key_Space);
    key(Qt::Key_Escape);
    QVERIFY(verbRuns().isEmpty());
    QCOMPARE(root->property("blocked").toBool(), false);
    QCOMPARE(root->property("focusRegion").toString(), QStringLiteral("list"));
    key('A');
}

// Type, tags and modification date narrow whatever the tab lists, each a
// criterion ANDed with the others and with `/`, held by the tab and shown as a
// chip that takes it off.
void TestStudio::narrowsAnyListingByTypeTagAndDate()
{
    QObject *root = m_engine->rootObjects().first();
    QFile old(QDir(m_tree.path()).filePath(QStringLiteral("plain.txt")));
    QVERIFY(old.open(QIODevice::ReadWrite));
    QVERIFY(old.setFileTime(QDateTime::currentDateTime().addDays(-400),
                            QFileDevice::FileModificationTime));
    const auto restore = qScopeGuard([&old] {
        old.setFileTime(QDateTime::currentDateTime(), QFileDevice::FileModificationTime);
        old.close();
    });
    m_index->reload();
    settle();
    settle();

    QObject *folder = filesRoot()->property("folder").value<QObject *>();
    const auto rows = [folder] {
        QStringList out;
        for (int i = 0; i < folder->property("count").toInt(); ++i) {
            QVariantMap entry;
            QMetaObject::invokeMethod(folder, "entryAt", Q_RETURN_ARG(QVariantMap, entry),
                                      Q_ARG(int, i));
            if (entry.value(QStringLiteral("kind")).toString() != QLatin1String("parent"))
                out.append(entry.value(QStringLiteral("name")).toString());
        }
        return out;
    };
    const auto chips = [this] {
        QStringList out;
        for (QQuickItem *chip : itemsNamed(QStringLiteral("criterionChip")))
            out.append(chip->property("label").toString());
        return out;
    };
    QVERIFY(rows().contains(QStringLiteral("alpha")));
    QVERIFY(rows().contains(QStringLiteral("one.txt")));
    QVERIFY(chips().isEmpty());

    // Folders only, from the menu under `F`; the same entry takes it off.
    key('F');
    QVERIFY(root->property("blocked").toBool());
    QVERIFY(chooseInMenu(QStringLiteral("filterMenu"), QStringLiteral("Only folders")));
    QVERIFY(rows().contains(QStringLiteral("alpha")));
    for (const QString &name : rows())
        QVERIFY2(QFileInfo(QDir(m_tree.path()).filePath(name)).isDir(), qPrintable(name));
    QCOMPARE(chips(), QStringList{QStringLiteral("folders  ×")});
    key('F');
    QVERIFY(chooseInMenu(QStringLiteral("filterMenu"), QStringLiteral("✓ Only folders")));
    QVERIFY(rows().contains(QStringLiteral("one.txt")));
    QVERIFY(chips().isEmpty());

    // The type is the Type column's word for the row under the cursor, and a
    // tag on that row narrows further: both have to hold.
    focusRow(QStringLiteral("one.txt"));
    key('F');
    QVERIFY(chooseInMenu(QStringLiteral("filterMenu"), QStringLiteral("Only txt")));
    QVERIFY(rows().contains(QStringLiteral("plain.txt")));
    for (const QString &name : rows())
        QVERIFY2(name.endsWith(QLatin1String(".txt")), qPrintable(name));
    focusRow(QStringLiteral("one.txt"));
    key('F');
    QVERIFY(chooseInMenu(QStringLiteral("filterMenu"), QStringLiteral("Only #work")));
    QCOMPARE(rows(), QStringList{QStringLiteral("one.txt")});

    // Taken off chip by chip, and all at once.
    const QList<QQuickItem *> shown = itemsNamed(QStringLiteral("criterionChip"));
    QCOMPARE(shown.size(), 2);
    QMetaObject::invokeMethod(shown.first(), "clicked");
    settle();
    QCOMPARE(chips().size(), 1);
    const QList<QQuickItem *> clear = itemsNamed(QStringLiteral("clearCriteria"));
    QCOMPARE(clear.size(), 1);
    QMetaObject::invokeMethod(clear.first(), "clicked");
    settle();
    QVERIFY(tab()->criteria().isEmpty());
    QVERIFY(chips().isEmpty());

    // The file's own date and not when it was tagged: `plain.txt` carries no
    // tag and was last written 400 days ago.
    tab()->toggleCriterion(QStringLiteral("tagged"), false);
    settle();
    QVERIFY(rows().contains(QStringLiteral("plain.txt")));
    QVERIFY(!rows().contains(QStringLiteral("one.txt")));
    tab()->toggleCriterion(QStringLiteral("within"), 31536000);
    settle();
    QVERIFY(!rows().contains(QStringLiteral("plain.txt")));
    QCOMPARE(chips(), (QStringList{QStringLiteral("untagged  ×"),
                                   QStringLiteral("modified in the last year  ×")}));
    QVERIFY(old.setFileTime(QDateTime::currentDateTime().addDays(-30),
                            QFileDevice::FileModificationTime));
    QMetaObject::invokeMethod(folder, "reload");
    settle();
    QVERIFY(rows().contains(QStringLiteral("plain.txt")));
    tab()->setCriteria({});

    // From the palette, and together with `/`.
    tab()->setFilter(QStringLiteral("o"));
    settle();
    key(':');
    for (char ch : QByteArray("only files"))
        key(ch);
    key(Qt::Key_Return);
    QVERIFY(rows().contains(QStringLiteral("two.md")));
    for (const QString &name : rows()) {
        QVERIFY2(name.contains(QLatin1Char('o'))
                     && QFileInfo(QDir(m_tree.path()).filePath(name)).isFile(),
                 qPrintable(name));
    }
    tab()->setFilter(QString());

    // Per tab: a new one starts unfiltered and this one keeps its own.
    m_workspace->addTab(QStringLiteral("files"));
    settle();
    QVERIFY(tab()->criteria().isEmpty());
    QVERIFY(chips().isEmpty());
    m_workspace->closeCurrentTab();
    settle();
    QCOMPARE(tab()->criteria().value(QStringLiteral("kind")).toString(), QStringLiteral("file"));
    // A duplicate is this tab again, criteria and all.
    m_workspace->duplicateTab(0, m_workspace->paneAtIndex(0)->currentIndex());
    settle();
    QCOMPARE(tab()->criteria().value(QStringLiteral("kind")).toString(), QStringLiteral("file"));
    m_workspace->closeCurrentTab();
    settle();

    // A tag listing is narrowed the same way.
    tab()->setSelectedTag(QStringLiteral("notes"));
    tab()->setCriteria({{QStringLiteral("type"), QStringLiteral("md")}});
    settle();
    QCOMPARE(rows(), (QStringList{QStringLiteral("six.md"), QStringLiteral("two.md")}));
    tab()->setSelectedTag(QString());

    // A search out of the lists of names draws only its first screenfuls, and
    // is narrowed before that cut: a folder past thousands of files is found.
    QTemporaryDir cache;
    QTemporaryDir shelf;
    QVERIFY(cache.isValid() && shelf.isValid());
    QVERIFY(QDir(shelf.path()).mkdir(QStringLiteral("zz")));
    const QByteArray previousCache = qgetenv("XDG_CACHE_HOME");
    qputenv("XDG_CACHE_HOME", cache.path().toUtf8());
    const QVector<Root> previousRoots =
        declareRoots({Root{QStringLiteral("shelf"), shelf.path(), false, QString()}});
    const auto unroot = qScopeGuard([&previousCache, &previousRoots] {
        qputenv("XDG_CACHE_HOME", previousCache);
        declareRoots(previousRoots);
    });
    const QDir names(QDir(cache.path()).filePath(QStringLiteral("omafiles/names")));
    QVERIFY(QDir().mkpath(names.absolutePath()));
    QFile list(names.filePath(QStringLiteral("shelf")));
    QVERIFY(list.open(QIODevice::WriteOnly | QIODevice::Text));
    for (int i = 0; i < 2100; ++i)
        list.write(QStringLiteral("f%1.txt\n").arg(i, 4, 10, QLatin1Char('0')).toUtf8());
    list.write("zz\n");
    list.close();
    tab()->setCriteria({{QStringLiteral("kind"), QStringLiteral("dir")}});
    tab()->setPattern(QStringLiteral("*"));
    settle();
    QCOMPARE(rows(), QStringList{QStringLiteral("zz")});
    tab()->setPattern(QString());
    tab()->setCriteria({});
    settle();
}

// A link row is the link: marked beside a file, it hands the command line its
// own name, and each verb applies its own symlink rule to it. `trash` and `rm`
// act on the name, so the target is left where it was.
void TestStudio::trashesAMarkedLinkByItsName()
{
    QTemporaryDir other;
    QVERIFY(other.isValid());
    const QDir dir(other.path());
    markALinkAndAFile(dir);
    // The target is a row of its own, and not marked with the link.
    QVERIFY(!tab()->isMarked(dir.filePath(QStringLiteral("a-target.txt"))));

    verbRuns().clear();
    key('m');
    QVERIFY(chooseInMenu(QStringLiteral("fileMenu"), QStringLiteral("Move 2 files to trash")));
    QCOMPARE(verbRuns().size(), 1);
    QCOMPARE(verbRuns().first(),
             (QStringList{"--yes", "trash", "--", dir.filePath(QStringLiteral("link")),
                          dir.filePath(QStringLiteral("plain.txt"))}));
}

// `mv` moves the link as the link it is.
void TestStudio::movesAMarkedLinkByItsName()
{
    QTemporaryDir other;
    QVERIFY(other.isValid());
    const QDir dir(other.path());
    markALinkAndAFile(dir);

    key('x', Qt::ControlModifier);
    verbRuns().clear();
    tab()->setCwd(m_tree.path());
    settle();
    key('v', Qt::ControlModifier);
    QTRY_VERIFY(itemsNamed("conflictSheet").first()->isVisible());
    key(Qt::Key_Tab);
    key(Qt::Key_Space);
    QTRY_COMPARE(verbRuns().size(), 2);
    QCOMPARE(verbRuns().at(0),
             (QStringList{"mv", "--", dir.filePath(QStringLiteral("link")), m_tree.path()}));
    QCOMPARE(verbRuns().at(1),
             (QStringList{"mv", "-T", "--keep-both", "--", dir.filePath(QStringLiteral("plain.txt")), m_tree.filePath("plain.txt")}));

    // One row renamed is `mv` too.
    tab()->setCwd(dir.path());
    settle();
    focusRow(QStringLiteral("link"));
    verbRuns().clear();
    key('r');
    QMetaObject::invokeMethod(m_engine->rootObjects().first(), "finishRename",
                              Q_ARG(QVariant, QVariant(true)),
                              Q_ARG(QVariant, QVariant(QStringLiteral("ln"))));
    settle();
    QCOMPARE(verbRuns().size(), 1);
    QCOMPARE(verbRuns().first(), (QStringList{"mv", "--", dir.filePath(QStringLiteral("link")),
                                              dir.filePath(QStringLiteral("ln"))}));
}

// `chmod` is handed the link, which it refuses by name rather than changing the
// target through it.
void TestStudio::changesPermissionsOfAMarkedLinkByItsName()
{
    QTemporaryDir other;
    QVERIFY(other.isValid());
    const QDir dir(other.path());
    markALinkAndAFile(dir);

    verbRuns().clear();
    key('m');
    QVERIFY(chooseInMenu(QStringLiteral("fileMenu"), QStringLiteral("Permissions of 2 files…")));
    settle();
    focusPermissionApply();
    key(Qt::Key_Return);
    QCOMPARE(verbRuns().size(), 1);
    QCOMPARE(verbRuns().first().first(), QStringLiteral("chmod"));
    QCOMPARE(verbRuns().first().mid(2),
             (QStringList{"--", dir.filePath(QStringLiteral("link")),
                          dir.filePath(QStringLiteral("plain.txt"))}));
}

// `cp` and the `tag` verbs resolve the link themselves, so they are handed its
// name like the rest; the row shows the tags its target carries.
void TestStudio::copiesAndTagsAMarkedLinkThroughItsName()
{
    QTemporaryDir other;
    QVERIFY(other.isValid());
    const QDir dir(other.path());
    const QString link = dir.filePath(QStringLiteral("link"));
    const QString plain = dir.filePath(QStringLiteral("plain.txt"));
    QFile index(QDir(m_home.path()).filePath(QStringLiteral("omafiles/index.jsonl")));
    QVERIFY(index.open(QIODevice::ReadOnly));
    const QByteArray before = index.readAll();
    index.close();
    const auto restore = qScopeGuard([this, &index, before] {
        if (index.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            index.write(before);
            index.close();
        }
        m_index->reload();
    settle();
    });
    QFile target(dir.filePath(QStringLiteral("a-target.txt")));
    QVERIFY(target.open(QIODevice::WriteOnly));
    target.close();
    tagDirectory(dir.filePath(QStringLiteral("a-target.txt")), QStringLiteral("linked"));
    markALinkAndAFile(dir);

    const QVariantMap entry = filesRoot()->property("entry").toMap();
    QCOMPARE(entry.value(QStringLiteral("path")).toString(), link);
    QCOMPARE(entry.value(QStringLiteral("tags")).toStringList(), QStringList{"linked"});

    key('t');
    const QString tag = m_index->tagAt(0);
    QVERIFY(!tag.isEmpty());
    key(Qt::Key_Escape);
    focusRow(QStringLiteral("link"));
    verbRuns().clear();
    key('m');
    QVERIFY(chooseInMenu(QStringLiteral("fileMenu"), QStringLiteral("Apply tag %1").arg(tag)));
    QCOMPARE(verbRuns().size(), 1);
    QCOMPARE(verbRuns().first(), (QStringList{"tag", "add", "--tag", tag, "--", link, plain}));

    key('c', Qt::ControlModifier);
    verbRuns().clear();
    tab()->setCwd(m_tree.path());
    settle();
    key('v', Qt::ControlModifier);
    QTRY_VERIFY(itemsNamed("conflictSheet").first()->isVisible());
    key(Qt::Key_Tab);
    key(Qt::Key_Space);
    QTRY_COMPARE(verbRuns().size(), 2);
    QCOMPARE(verbRuns().at(0), (QStringList{"cp", "--", link, m_tree.path()}));
    QCOMPARE(verbRuns().at(1), (QStringList{"cp", "-T", "--keep-both", "--", plain, m_tree.filePath("plain.txt")}));
}



// Synthetic widths exercise joins even with only one allowed CPU.
static QVector<int> testDirectoryCpus(int workers)
{
    const auto allowed = workerCpus();
    QVector<int> result;
    for (int i = 0; i < workers; ++i) result.append(allowed.at(i % allowed.size()));
    return result;
}

void TestStudio::keepsSingleWorkerDirectoryWorkInline()
{
    DirectoryWork work(testDirectoryCpus(1));
    QCOMPARE(work.workerLimit(), 1);
    const auto parts = work.chunks(1000000);
    QCOMPARE(parts.size, 1);
    const auto caller = QThread::currentThreadId();
    int visits = 0;
    work.run(parts, [&](int part, qsizetype first, qsizetype last) {
        QCOMPARE(QThread::currentThreadId(), caller);
        QCOMPARE(part, 0); QCOMPARE(first, 0); QCOMPARE(last, 1000000);
        ++visits;
    });
    QCOMPARE(visits, 1);
    DirectoryWork parallel(testDirectoryCpus(2));
    const auto small = parallel.chunks(DirectoryWork::minimumChunkSize - 1);
    QCOMPARE(small.size, 1);
    parallel.run(small, [&](int, qsizetype, qsizetype) {
        QCOMPARE(QThread::currentThreadId(), caller);
    });
    QCOMPARE(parallel.chunks(100000, DirectoryWork::minimumFilterChunkSize).size, 1);
    QCOMPARE(parallel.chunks(1000000, DirectoryWork::minimumFilterChunkSize).size, 2);
    QCOMPARE(directoryWork()->workerLimit(), workerCpus().size());
}

void TestStudio::runsDirectoryChunksWithinWorkerBudget()
{
    auto work = std::make_shared<DirectoryWork>(testDirectoryCpus(2));
    QCOMPARE(work->workerLimit(), 2);
    const auto parts = work->chunks(65537);
    QCOMPARE(parts.size, 2);
    QCOMPARE(parts.ranges[0].first, 0);
    QCOMPARE(parts.ranges[0].last, parts.ranges[1].first);
    QCOMPARE(parts.ranges[1].last, 65537);
    QSemaphore entered, release;
    std::atomic_int active{0}, maximum{0}, completed{0};
    auto future = QtConcurrent::run([&] {
        work->run(parts, [&](int, qsizetype, qsizetype) {
            const int current = active.fetch_add(1) + 1;
            int previous = maximum.load();
            while (previous < current && !maximum.compare_exchange_weak(previous, current)) {}
            entered.release(); release.acquire();
            --active; ++completed;
        });
    });
    const auto finish = qScopeGuard([&] { release.release(2); future.waitForFinished(); });
    QVERIFY(entered.tryAcquire(2, asyncTimeout));
    QCOMPARE(active.load(), 2);
    release.release(2);
    QTRY_VERIFY(future.isFinished());
    QCOMPARE(maximum.load(), 2); QCOMPARE(completed.load(), 2);
}

void TestStudio::bindsDirectoryChunksToDistinctCpus()
{
    auto cpus = workerCpus();
    if (cpus.size() < 2) QSKIP("Two allowed CPUs are needed for distinct placement");
    cpus.resize(2);
    cpu_set_t original;
    QCOMPARE(::sched_getaffinity(0, sizeof(original), &original), 0);
    const auto restore = qScopeGuard([&] { ::sched_setaffinity(0, sizeof(original), &original); });
    DirectoryWork work(cpus);
    QSemaphore entered, release;
    std::array<int, 2> observed{{-1, -1}}, widths{{0, 0}};
    auto future = QtConcurrent::run([&] {
        work.run(work.chunks(131072), [&](int part, qsizetype, qsizetype) {
            cpu_set_t mask;
            if (::sched_getaffinity(0, sizeof(mask), &mask) == 0) widths[part] = CPU_COUNT(&mask);
            observed[part] = ::sched_getcpu();
            entered.release(); release.acquire();
        });
    });
    const auto finish = qScopeGuard([&] { release.release(2); future.waitForFinished(); });
    QVERIFY(entered.tryAcquire(2, asyncTimeout));
    QCOMPARE(widths[0], 1); QCOMPARE(widths[1], 1);
    QVERIFY(cpus.contains(observed[0]) && cpus.contains(observed[1]));
    QVERIFY(observed[0] != observed[1]);
    cpu_set_t caller;
    QCOMPARE(::sched_getaffinity(0, sizeof(caller), &caller), 0);
    QVERIFY(CPU_EQUAL(&caller, &original));
    release.release(2);
    QTRY_VERIFY(future.isFinished());
}

void TestStudio::releasesDirectoryCpusAfterFailures()
{
    DirectoryWork work(testDirectoryCpus(2));
    const auto parts = work.chunks(131072);
    QVERIFY_EXCEPTION_THROWN(work.run(parts, [&](int, qsizetype, qsizetype) {
        throw std::runtime_error("directory CPU release test");
    }), std::runtime_error);
    bool recovered = true;
    std::atomic_int visits{0};
    try { work.run(parts, [&](int, qsizetype, qsizetype) { ++visits; }); }
    catch (...) { recovered = false; }
    QVERIFY(recovered);
    QCOMPARE(visits.load(), 2);
}

void TestStudio::mergesParallelDirectorySorts_data()
{
    QTest::addColumn<int>("workers");
    for (int workers : {1, 2, 3, 4})
        QTest::newRow(qPrintable(QString::number(workers))) << workers;
}

void TestStudio::mergesParallelDirectorySorts()
{
    QFETCH(int, workers);
    DirectoryWork work(testDirectoryCpus(workers));
    QVector<int> values(131077);
    for (int i = 0; i < values.size(); ++i) values[i] = (i * 1597) % 131077;
    QVector<int> expected = values;
    std::sort(expected.begin(), expected.end());
    QVERIFY(work.sort(values.begin(), values.end(), std::less<int>()));
    QCOMPARE(values, expected);
    QVERIFY(work.sort(values.begin(), values.end(), std::greater<int>()));
    std::reverse(expected.begin(), expected.end());
    QCOMPARE(values, expected);
    auto cancellation = std::make_shared<std::atomic_bool>(true);
    QVERIFY(!work.sort(values.begin(), values.end(), std::less<int>(), cancellation));
    QCOMPARE(values, expected);
    QVector<int> empty;
    QVERIFY(work.sort(empty.begin(), empty.end(), std::less<int>()));
    QVERIFY(empty.isEmpty());
}

void TestStudio::avoidsMergingOrderedDirectoryChunks_data()
{
    QTest::addColumn<int>("workers");
    QTest::addColumn<bool>("descending");
    for (int workers : {2, 3, 4})
        for (bool descending : {false, true})
            QTest::newRow(qPrintable(QString("%1-%2").arg(workers).arg(descending)))
                << workers << descending;
}

void TestStudio::avoidsMergingOrderedDirectoryChunks()
{
    QFETCH(int, workers);
    QFETCH(bool, descending);
    DirectoryWork work(testDirectoryCpus(workers));
    QVector<int> values(131077);
    for (int i = 0; i < values.size(); ++i) values[i] = descending ? -i : i;
    const auto parts = work.chunks(values.size());
    QVector<int> reference = values;
    qsizetype sortingComparisons = 0;
    const auto less = [descending](int a, int b) { return descending ? a > b : a < b; };
    for (int part = 0; part < parts.size; ++part)
        std::sort(reference.begin() + parts.ranges[part].first,
                  reference.begin() + parts.ranges[part].last, [&](int a, int b) {
            ++sortingComparisons; return less(a, b);
        });
    std::atomic<qsizetype> comparisons{0};
    QVERIFY(work.sort(values.begin(), values.end(), [&](int a, int b) {
        ++comparisons; return less(a, b);
    }));
    QCOMPARE(values, reference);
    QVERIFY2(comparisons.load() <= sortingComparisons + parts.size - 1,
             "Ordered chunks must need only boundary comparisons after their own sorts");
}

void TestStudio::propagatesDirectoryChunkFailures()
{
    DirectoryWork work(testDirectoryCpus(2));
    const auto parts = work.chunks(65536);
    std::atomic_int visits{0};
    QVERIFY_EXCEPTION_THROWN(work.run(parts, [&](int, qsizetype, qsizetype) {
        ++visits; throw std::runtime_error("directory work test failure");
    }), std::runtime_error);
    QCOMPARE(visits.load(), 2);
    work.run(parts, [&](int, qsizetype, qsizetype) { ++visits; });
    QCOMPARE(visits.load(), 4);
}

void TestStudio::cancelsPendingDirectoryChunks()
{
    auto work = std::make_shared<DirectoryWork>(testDirectoryCpus(2));
    const auto parts = work->chunks(65536);
    auto cancellation = std::make_shared<std::atomic_bool>(true);
    std::atomic_int visits{0};
    work->run(parts, [&](int, qsizetype, qsizetype) { ++visits; }, cancellation);
    QCOMPARE(visits.load(), 0);
    cancellation->store(false);
    QSemaphore entered, release;
    auto active = QtConcurrent::run([&] {
        work->run(parts, [&](int, qsizetype, qsizetype) { entered.release(); release.acquire(); });
    });
    const auto finish = qScopeGuard([&] { release.release(2); active.waitForFinished(); });
    QVERIFY(entered.tryAcquire(2, asyncTimeout));
    auto pending = QtConcurrent::run([&] {
        work->run(parts, [&](int, qsizetype, qsizetype) { ++visits; }, cancellation);
    });
    const auto join = qScopeGuard([&] { release.release(2); pending.waitForFinished(); });
    cancellation->store(true);
    release.release(2);
    QTRY_VERIFY(active.isFinished() && pending.isFinished());
    QCOMPARE(visits.load(), 0);
}

void TestStudio::skipsPartitionPlanningForSmallDirectoryQueries()
{
    QTemporaryDir tree;
    QVERIFY(tree.isValid());
    for (int i = 0; i < 100000; ++i) {
        QFile file(tree.filePath(QStringLiteral("file-%1.txt").arg(i, 6, 10, QLatin1Char('0'))));
        QVERIFY(file.open(QIODevice::WriteOnly));
    }
    TabState state("files"); state.setCwd(tree.path());
    for (const int workers : {1, 2}) {
        auto work = std::make_shared<const DirectoryWork>(testDirectoryCpus(workers));
        FolderModel model(nullptr, work);
        model.setSource(m_index); model.setTab(&state);
        QTRY_VERIFY(!model.loading());
        const unsigned partitions = work->partitionPasses();
        const int reads = model.listingReads();
        model.setFilter("file-099999.txt");
        QTRY_VERIFY(!model.loading());
        QCOMPARE(model.queryEntriesExamined(), 100000);
        QCOMPARE(model.rowCount(), 2);
        QCOMPARE(model.pathAt(1), tree.filePath("file-099999.txt"));
        QCOMPARE(model.listingReads(), reads);
        QCOMPARE(work->partitionPasses(), partitions);
    }
}

void TestStudio::matchesParallelDirectoryQueries()
{
    QTemporaryDir tree;
    QVERIFY(tree.isValid());
    for (int i = 0; i < 131072; ++i) {
        QFile file(tree.filePath(QStringLiteral("file-%1.txt").arg(i, 6, 10, QLatin1Char('0'))));
        QVERIFY(file.open(QIODevice::WriteOnly));
        if (i % 16 == 0) QCOMPARE(file.write("content"), 7);
    }
    const QStringList extra{QStringLiteral(".hidden.txt"), QStringLiteral("\u03a3.txt"),
        QStringLiteral("\u212a.txt"), QStringLiteral("\U0001f642.txt")};
    for (const QString &name : extra) { QFile file(tree.filePath(name)); QVERIFY(file.open(QIODevice::WriteOnly)); }
    QVERIFY(QDir(tree.path()).mkdir("a-folder"));
    QVERIFY(QFile::link(tree.filePath("a-folder"), tree.filePath("b-link")));
    QVERIFY(QFile::link(tree.filePath("missing"), tree.filePath("dangling")));
    auto serial = std::make_shared<const DirectoryWork>(testDirectoryCpus(1));
    auto parallel = std::make_shared<const DirectoryWork>(testDirectoryCpus(2));
    const auto left = DirectoryListing::read(tree.path(), {}, serial);
    const auto right = DirectoryListing::read(tree.path(), {}, parallel);
    QCOMPARE(left.size(), right.size());
    for (qsizetype i = 0; i < left.size(); ++i) {
        QCOMPARE(left.name(i).toString(), right.name(i).toString());
        QCOMPARE(left.isDirectory(i), right.isDirectory(i));
        QCOMPARE(left.isLink(i), right.isLink(i));
    }
    QCOMPARE(left.nameStorageCapacityBytes(), right.nameStorageCapacityBytes());
    QCOMPARE(left.entryStorageBytes(), right.entryStorageBytes());
    QCOMPARE(left.metadataReads, right.metadataReads);
    QFile index(QDir(m_home.path()).filePath("omafiles/index.jsonl"));
    QVERIFY(index.open(QIODevice::ReadOnly)); const QByteArray original = index.readAll(); index.close();
    const auto restore = qScopeGuard([&] {
        if (index.open(QIODevice::WriteOnly | QIODevice::Truncate)) { index.write(original); index.close(); }
        m_index->reload();
    });
    const QString tagged = tree.filePath("file-000123.txt");
    tagDirectory(tagged, "parallel-test", false);
    tagDirectory(tree.filePath("file-060000.txt"), "parallel-other");
    TabState state("files"); state.setCwd(tree.path());
    FolderModel a(nullptr, serial), b(nullptr, parallel);
    a.setSource(m_index); b.setSource(m_index);
    a.setTab(&state); b.setTab(&state);
    QTRY_VERIFY(!a.loading() && !b.loading());
    const int readsA = a.listingReads(), readsB = b.listingReads();
    const auto compare = [&] {
        if (a.rowCount() != b.rowCount() || a.taggedCount() != b.taggedCount()) return false;
        for (int row = 0; row < a.rowCount(); ++row)
            if (a.pathAt(row) != b.pathAt(row)) return false;
        return a.queryEntriesExamined() == b.queryEntriesExamined();
    };
    QVERIFY(compare());
    QCOMPARE(a.tagsFor(tagged), (QStringList{"parallel-test"}));
    QCOMPARE(b.tagsFor(tagged), (QStringList{"parallel-test"}));
    const unsigned serialPartitions = serial->partitionPasses();
    const unsigned parallelPartitions = parallel->partitionPasses();
    for (const QString &needle : {QStringLiteral("file-00"), QStringLiteral("file-000"),
            QStringLiteral("file-000123.txt"), QStringLiteral("TXT"), QStringLiteral("\u03c3"),
            QStringLiteral("\U0001f642"), QStringLiteral("no-match")}) {
        a.setFilter(needle); b.setFilter(needle);
        QTRY_VERIFY(!a.loading() && !b.loading()); QVERIFY(compare());
        QCOMPARE(serial->partitionPasses(), serialPartitions);
    }
    QVERIFY(parallel->partitionPasses() > parallelPartitions);
    a.setFilter("file-00"); b.setFilter("file-00");
    a.setSortAscending(false); b.setSortAscending(false);
    QTRY_VERIFY(!a.loading() && !b.loading()); QVERIFY(compare());
    a.setShowHidden(true); b.setShowHidden(true);
    a.setFilter(".hidden"); b.setFilter(".hidden");
    QTRY_VERIFY(!a.loading() && !b.loading()); QVERIFY(compare());
    a.setFilter("file-"); b.setFilter("file-");
    a.setSortRole("size"); b.setSortRole("size");
    a.setCriteria({{"kind", "file"}}); b.setCriteria({{"kind", "file"}});
    QTRY_VERIFY(!a.loading() && !b.loading()); QVERIFY(compare());
    QCOMPARE(a.listingReads(), readsA); QCOMPARE(b.listingReads(), readsB);
    QTemporaryDir other;
    a.setFilter("file-"); b.setFilter("file-"); state.setCwd(other.path());
    QTRY_VERIFY(!a.loading() && !b.loading());
    QThreadPool::globalInstance()->waitForDone(); QCoreApplication::processEvents();
    QCOMPARE(a.rowCount(), 1); QCOMPARE(b.rowCount(), 1);
    QCOMPARE(a.directory(), other.path()); QCOMPARE(b.directory(), other.path());
}

void TestStudio::coalescesPendingDirectoryNotifications()
{
    QTemporaryDir tree, other;
    QVERIFY(tree.isValid() && other.isValid());
    for (const QString name : {"alpha.txt", "beta.txt"}) {
        QFile file(tree.filePath(name));
        QVERIFY(file.open(QIODevice::WriteOnly));
    }
    TabState state("files");
    state.setCwd(tree.path());
    FolderModel model;
    model.setTab(&state);
    QTRY_VERIFY(!model.loading());
    const int reads = model.listingReads();
    QSignalSpy changed(&model, &FolderModel::changed);
    QSignalSpy query(&model, &FolderModel::queryChanged);
    for (const QString text : {"a", "al", "alp", "alph", "alpha"})
        model.setFilter(text);
    QCOMPARE(query.size(), 5);
    QCOMPARE(changed.size(), 1);
    QVERIFY(model.loading());
    QTRY_VERIFY(!model.loading());
    QCOMPARE(changed.size(), 2);
    QCOMPARE(model.rowCount(), 2);
    QCOMPARE(model.pathAt(1), tree.filePath("alpha.txt"));
    QCOMPARE(model.listingReads(), reads);
    changed.clear();
    model.setFilter("b");
    state.setCwd(other.path());
    QCOMPARE(changed.size(), 2);
    QCOMPARE(model.rowCount(), 1);
    QTRY_VERIFY(!model.loading());
    QCOMPARE(changed.size(), 3);
    QCOMPARE(model.rowCount(), 1);
    QCOMPARE(model.directory(), other.path());
}

// The folder follows entries created, renamed and removed by other programs.
// Its watch asks for nothing about the entries themselves: such events make the
// kernel flag every cached child when the watch is added.
void TestStudio::followsDirectoryEntriesOnDisk()
{
    QTemporaryDir tree;
    QVERIFY(tree.isValid());
    QVERIFY(QFile(tree.filePath("alpha.txt")).open(QIODevice::WriteOnly));
    QHash<QString, uint32_t> masks;
    registeredMasks = &masks;
    const auto unregister = qScopeGuard([] { registeredMasks = nullptr; });
    TabState state("files");
    state.setCwd(tree.path());
    FolderModel model;
    model.setTab(&state);
    QTRY_VERIFY(!model.loading());
    QVERIFY(masks.contains(tree.path()));
    QCOMPARE(masks.value(tree.path()) & (IN_CREATE | IN_DELETE | IN_MOVE | IN_DELETE_SELF),
             uint32_t(IN_CREATE | IN_DELETE | IN_MOVE | IN_DELETE_SELF));
    QCOMPARE(masks.value(tree.path()) & (IN_ATTRIB | IN_MODIFY | IN_CLOSE_WRITE | IN_OPEN | IN_ACCESS),
             uint32_t(0));
    const auto rows = [&model] {
        QStringList paths;
        for (int row = 0; row < model.rowCount(); ++row)
            paths.append(model.pathAt(row));
        return paths;
    };
    QVERIFY(QFile(tree.filePath("beta.txt")).open(QIODevice::WriteOnly));
    QTRY_VERIFY(rows().contains(tree.filePath("beta.txt")));
    QVERIFY(QFile::rename(tree.filePath("beta.txt"), tree.filePath("gamma.txt")));
    QTRY_VERIFY(rows().contains(tree.filePath("gamma.txt")));
    QVERIFY(!rows().contains(tree.filePath("beta.txt")));
    QVERIFY(QFile::remove(tree.filePath("gamma.txt")));
    QTRY_VERIFY(!rows().contains(tree.filePath("gamma.txt")));
    QVERIFY(rows().contains(tree.filePath("alpha.txt")));
}

// A directory watch reports its own entries and asks nothing about the entries
// themselves; a file watch reports edits and forgets the file once replaced.
void TestStudio::watchesDirectoryEntriesAndFileContents()
{
    QTemporaryDir tree;
    QVERIFY(tree.isValid());
    const QString file = tree.filePath("notes.txt");
    QVERIFY(QFile(file).open(QIODevice::WriteOnly));
    QHash<QString, uint32_t> masks;
    registeredMasks = &masks;
    const auto unregister = qScopeGuard([] { registeredMasks = nullptr; });
    EntryWatcher watcher;
    QSignalSpy directories(&watcher, &EntryWatcher::directoryChanged);
    QSignalSpy files(&watcher, &EntryWatcher::fileChanged);
    QVERIFY(watcher.addPath(tree.path()));
    QVERIFY(watcher.addPath(file));
    QVERIFY(!watcher.addPath(tree.filePath("missing")));
    QCOMPARE(masks.value(tree.path()) & (IN_ATTRIB | IN_MODIFY | IN_CLOSE_WRITE | IN_OPEN | IN_ACCESS),
             uint32_t(0));
    QCOMPARE(masks.value(tree.path()) & (IN_CREATE | IN_DELETE | IN_MOVE | IN_DELETE_SELF),
             uint32_t(IN_CREATE | IN_DELETE | IN_MOVE | IN_DELETE_SELF));
    QCOMPARE(masks.value(file) & (IN_MODIFY | IN_ATTRIB | IN_DELETE_SELF | IN_MOVE_SELF),
             uint32_t(IN_MODIFY | IN_ATTRIB | IN_DELETE_SELF | IN_MOVE_SELF));
    {
        QFile edit(file);
        QVERIFY(edit.open(QIODevice::Append));
        QVERIFY(edit.write("edit") == 4);
    }
    QTRY_VERIFY(files.contains(QVariantList{file}));
    QVERIFY(directories.isEmpty());
    QVERIFY(QFile(tree.filePath("draft.txt")).open(QIODevice::WriteOnly));
    QTRY_VERIFY(directories.contains(QVariantList{tree.path()}));
    files.clear();
    QVERIFY(::rename(QFile::encodeName(tree.filePath("draft.txt")).constData(),
                     QFile::encodeName(file).constData()) == 0);
    QTRY_VERIFY(!watcher.contains(file));
    QVERIFY(files.contains(QVariantList{file}));
    QVERIFY(watcher.contains(tree.path()));
    watcher.removePath(tree.path());
    QVERIFY(!watcher.contains(tree.path()));
}

void TestStudio::keepsLargeFolderWorkBounded()
{
    QTemporaryDir tree;
    for (int i = 0; i < 4096; ++i) {
        QFile file(tree.filePath(QStringLiteral("file-%1.txt").arg(i, 6, 10, QLatin1Char('0'))));
        QVERIFY(file.open(QIODevice::WriteOnly));
    }
    const DirectoryListing snapshot = DirectoryListing::read(tree.path());
    QCOMPARE(snapshot.size(), 4096);
    QCOMPARE(snapshot.metadataReads, 0);
    QCOMPARE(snapshot.name(4095).toString(), QStringLiteral("file-004095.txt"));
    auto cancellation = std::make_shared<std::atomic_bool>(true);
    const auto cancelled = DirectoryListing::read(tree.path(), cancellation);
    QVERIFY(cancelled.cancelled);
    QCOMPARE(cancelled.size(), 0);

    TabState state(QStringLiteral("files"));
    state.setCwd(tree.path());
    FolderModel model;
    model.setTab(&state);
    QVERIFY(model.loading());
    QCOMPARE(model.rowCount(), 1);
    QTRY_VERIFY(!model.loading());
    QCOMPARE(model.rowCount(), 4097);
    QCOMPARE(model.metadataReads(), 0);
    const int reads = model.listingReads();
    const int last = model.rowForPath(tree.filePath("file-004095.txt"));
    QCOMPARE(last, 4096);
    QCOMPARE(model.data(model.index(last), FolderModel::NameRole).toString(), QStringLiteral("file-004095.txt"));
    QCOMPARE(model.metadataReads(), 0);
    QCOMPARE(model.data(model.index(last), FolderModel::TypeRole).toString(), QStringLiteral("txt"));
    QCOMPARE(model.metadataReads(), 1);
    model.data(model.index(last), FolderModel::ModeRole);
    QCOMPARE(model.metadataReads(), 1);
    for (int i = 1; i < 300; ++i)
        model.data(model.index(i), FolderModel::TypeRole);
    const int described = model.metadataReads();
    model.data(model.index(last), FolderModel::TypeRole);
    QCOMPARE(model.metadataReads(), described + 1);

    model.setFilter("file-");
    model.setFilter("file-004");
    model.setFilter("file-004095.txt");
    QVERIFY(model.loading());
    QTRY_VERIFY(!model.loading());
    QCOMPARE(model.listingReads(), reads);
    QCOMPARE(model.rowCount(), 2);
    QCOMPARE(model.pathAt(1), tree.filePath("file-004095.txt"));
    QCOMPARE(model.metadataReads(), described + 1);
    model.setFilter("");
    QTRY_VERIFY(!model.loading());
    QCOMPARE(model.rowCount(), 4097);

    QTemporaryDir other;
    model.reload();
    QCoreApplication::processEvents();
    state.setCwd(other.path());
    QTRY_VERIFY(!model.loading());
    QThreadPool::globalInstance()->waitForDone();
    QCoreApplication::processEvents();
    QCOMPARE(model.directory(), other.path());
    QCOMPARE(model.rowCount(), 1);
    QCOMPARE(model.kindAt(0), QStringLiteral("parent"));
}

void TestStudio::preservesMixedEncodingDirectoryNames()
{
    QTemporaryDir tree;
    const QStringList names{QStringLiteral("ascii.txt"), QStringLiteral("\u00c9cole.txt"),
                            QStringLiteral("\u0394elta.txt"), QStringLiteral("\U0001f642.txt"),
                            QStringLiteral("K.txt"), QStringLiteral("s.txt"), QStringLiteral("\u00b5.txt"),
                            QStringLiteral(".\u00e9-hidden"), QStringLiteral(".\u03b4-hidden"),
                            QString(255, QLatin1Char('x')), QStringLiteral("line\nwith-tab\t"),
                            QStringLiteral("\ufeffleading-bom")};
    const auto removeNames = qScopeGuard([&] {
        for (const QString &name : names) QFile::remove(tree.filePath(name));
    });
    for (const QString &name : names) {
        QFile file(tree.filePath(name));
        QVERIFY(file.open(QIODevice::WriteOnly));
    }
    QVERIFY(QDir(tree.path()).mkdir(QStringLiteral("\u03b2-folder")));
    QVERIFY(QFile::link(tree.filePath(QStringLiteral("\u03b2-folder")),
                        tree.filePath(QStringLiteral("latin-link"))));
    const auto listing = DirectoryListing::read(tree.path());
    QCOMPARE(listing.size(), names.size() + 2);
    QVERIFY(listing.isDirectory(0));
    QVERIFY(listing.isDirectory(1));
    QStringList actual;
    for (qsizetype i = 0; i < listing.size(); ++i) {
        const QString name = listing.name(i).toString();
        actual.append(name);
        const bool compact = std::all_of(name.cbegin(), name.cend(),
                                         [](QChar ch) { return ch.unicode() <= 0x7f; });
        QCOMPARE(listing.name(i).size_bytes(), name.size() * (compact ? 1 : 2));
        QCOMPARE(listing.isLink(i), name == QStringLiteral("latin-link"));
        for (const QString &needle : {QString(), QStringLiteral("\u00e9"),
                                      QStringLiteral("\u03b4"), QStringLiteral("TXT"),
                                      QStringLiteral("\u212a"), QStringLiteral("\u017f"), QStringLiteral("\u03bc"),
                                      QStringLiteral("\U0001f642")}) {
            for (bool hidden : {false, true})
                QCOMPARE(listing.matches(i, DirectoryNameQuery(needle), hidden),
                         (hidden || !name.startsWith('.'))
                             && name.contains(needle, Qt::CaseInsensitive));
        }
        if (i > 0 && listing.isDirectory(i) == listing.isDirectory(i - 1))
            QVERIFY(actual.at(i - 1).compare(name, Qt::CaseInsensitive) <= 0);
    }
    for (const QString &name : names)
        QVERIFY(actual.contains(name));
}

// ASCII punctuation between 'Z' and 'a' tells folding to lower case apart
// from folding to upper case or comparing bytes.
void TestStudio::sortsAsciiNamesInQtCaseInsensitiveOrder()
{
    QTemporaryDir tree;
    QVERIFY(tree.isValid());
    const QStringList names{"_x", "[x", "\\x", "]x", "^x", "`x", "@x", "~x", "{x", "Ax", "ax",
        "Bx", "zx", "Zy", "a", "A1", "a10", "a2", "ab", "aB", "AC", QStringLiteral("\u00e9x"),
        QStringLiteral("\u00c9y")};
    for (const QString &name : names)
        QVERIFY(QFile(tree.filePath(name)).open(QIODevice::WriteOnly));
    const auto listing = DirectoryListing::read(tree.path());
    QCOMPARE(listing.size(), names.size());
    for (qsizetype i = 1; i < listing.size(); ++i)
        QVERIFY2(listing.name(i - 1).toString().compare(listing.name(i).toString(), Qt::CaseInsensitive) <= 0,
                 qPrintable(listing.name(i - 1).toString() + " > " + listing.name(i).toString()));
}

void TestStudio::preparesDirectoryQueriesWithoutChangingUnicodeMatching()
{
    QTemporaryDir tree;
    QVERIFY(tree.isValid());
    const QStringList names{"K.txt", "s.txt", "FOO.txt", ".hidden.txt",
        QStringLiteral("\u212aelvin.txt"), QStringLiteral("\u017fample.txt"),
        QStringLiteral("\u03a3.txt"), QStringLiteral("\u03c3.txt"), QStringLiteral("\u03c2.txt"),
        QStringLiteral("\u1e9e.txt"), QStringLiteral("\u00df.txt"), QStringLiteral("\u00b5.txt"),
        QStringLiteral("\u03bc.txt"), QStringLiteral("\U00010400.txt"), QStringLiteral("\U00010428.txt"),
        QStringLiteral("\U0001f642.txt"), QStringLiteral("\u00e9.txt"), QStringLiteral("e\u0301.txt"),
        QString(250, QLatin1Char('a')) + ".txt"};
    for (const QString &name : names) {
        QFile file(tree.filePath(name));
        QVERIFY(file.open(QIODevice::WriteOnly));
    }
    const auto listing = DirectoryListing::read(tree.path());
    QCOMPARE(listing.size(), names.size());
    const QStringList needles{"", "foo", "FOO", ".txt", "SS", " ",
        QStringLiteral("\u212a"), QStringLiteral("\u017f"), QStringLiteral("\u03c3"),
        QStringLiteral("\u03bc"), QStringLiteral("\u00b5"), QStringLiteral("\u00df"),
        QStringLiteral("\U00010400"), QStringLiteral("\U00010428"), QStringLiteral("\U0001f642"),
        QStringLiteral("\u00e9"), QStringLiteral("e\u0301"), QString(256, QLatin1Char('a'))};
    for (const QString &needle : needles) {
        const DirectoryNameQuery query(needle);
        for (int i = 0; i < listing.size(); ++i) {
            const QString name = listing.name(i).toString();
            for (bool hidden : {false, true}) {
                QCOMPARE(listing.matches(i, query, hidden),
                    (hidden || !name.startsWith('.')) && name.contains(needle, Qt::CaseInsensitive));
            }
        }
    }
    QString needle = QString::fromUtf8("\xe2\x84\xaa");
    const DirectoryNameQuery snapshot(needle);
    needle[0] = QChar(0x17f);
    for (int i = 0; i < listing.size(); ++i)
        QCOMPARE(listing.matches(i, snapshot, true),
            listing.name(i).toString().contains(QStringLiteral("\u212a"), Qt::CaseInsensitive));
}

void TestStudio::narrowsCompletedNameQueries_data()
{
    QTest::addColumn<bool>("ascending");
    QTest::newRow("ascending") << true;
    QTest::newRow("descending") << false;
}

void TestStudio::narrowsCompletedNameQueries()
{
    QFETCH(bool, ascending);
    QTemporaryDir tree;
    QVERIFY(tree.isValid());
    const QStringList names{"alpha-one.txt", "alpha-two.txt", "alpine.txt", "beta.txt", ".alpha-hidden", "z.txt", "alpha-dir"};
    for (const QString &name : names) {
        if (name == "alpha-dir") QVERIFY(QDir(tree.path()).mkdir(name));
        else { QFile file(tree.filePath(name)); QVERIFY(file.open(QIODevice::WriteOnly)); }
    }
    TabState state("files");
    state.setCwd(tree.path());
    FolderModel model;
    model.setTab(&state);
    model.setSortAscending(ascending);
    QTRY_VERIFY(!model.loading());
    const int reads = model.listingReads();
    const auto filter = [&](const QString &text, int examined) {
        model.setFilter(text);
        QTRY_VERIFY(!model.loading());
        QCOMPARE(model.queryEntriesExamined(), examined);
        QStringList expected;
        for (const QString &name : names)
            if ((model.showHidden() || !name.startsWith('.'))
                && name.contains(text.trimmed(), Qt::CaseInsensitive)) expected.append(name);
        std::sort(expected.begin(), expected.end(), [&](const QString &a, const QString &b) {
            if ((a == "alpha-dir") != (b == "alpha-dir")) return a == "alpha-dir";
            const int order = a.compare(b, Qt::CaseInsensitive);
            return model.sortAscending() ? order < 0 : order > 0;
        });
        QCOMPARE(model.kindAt(0), QStringLiteral("parent"));
        QStringList actual;
        for (int row = 1; row < model.rowCount(); ++row) {
            QVERIFY(model.kindAt(row) != "parent");
            actual.append(model.data(model.index(row), FolderModel::NameRole).toString());
        }
        QCOMPARE(actual, expected);
        QCOMPARE(model.listingReads(), reads);
    };
    filter("al", names.size());
    filter("alp", 4);
    filter("alpha-", 4);
    filter("ALPHA-O", 3);
    filter("al", names.size());
    filter("be", names.size());
    filter("", names.size());
    filter(" al ", names.size());
    filter("  alp  ", 4);
    model.setShowHidden(true);
    QTRY_VERIFY(!model.loading());
    QCOMPARE(model.queryEntriesExamined(), names.size());
    filter("alpha", 5);
    model.setSortAscending(!ascending);
    QTRY_VERIFY(!model.loading());
    QCOMPARE(model.queryEntriesExamined(), names.size());
    filter("alpha-", 4);
    QCOMPARE(model.metadataReads(), 0);
}

void TestStudio::invalidatesNarrowingForMetadataAndStorage()
{
    QTemporaryDir tree;
    QVERIFY(tree.isValid());
    for (const QString name : {"alpha.txt", "alpine.md", "beta.txt"}) {
        QFile file(tree.filePath(name));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("content");
    }
    QVERIFY(QDir(tree.path()).mkdir("alpha-dir"));
    TabState state("files");
    state.setCwd(tree.path());
    FolderModel model;
    model.setSource(m_index);
    model.setTab(&state);
    QTRY_VERIFY(!model.loading());
    model.setFilter("al"); QTRY_VERIFY(!model.loading());
    QCOMPARE(model.queryEntriesExamined(), 4);
    model.setSortRole("size"); QTRY_VERIFY(!model.loading());
    model.setFilter("alp"); QTRY_VERIFY(!model.loading());
    QCOMPARE(model.queryEntriesExamined(), 4);
    model.setSortRole("name");
    model.setCriteria({{"kind", "file"}}); QTRY_VERIFY(!model.loading());
    model.setFilter("alpha"); QTRY_VERIFY(!model.loading());
    QCOMPARE(model.queryEntriesExamined(), 4);
    QCOMPARE(model.rowCount(), 2);
    model.setCriteria({}); QTRY_VERIFY(!model.loading());
    QCOMPARE(model.queryEntriesExamined(), 4);
    QCOMPARE(model.rowCount(), 3);
    QFile added(tree.filePath("alpha-new.txt"));
    QVERIFY(added.open(QIODevice::WriteOnly)); added.close();
    const int reads = model.listingReads();
    model.reload(); QTRY_VERIFY(!model.loading());
    QCOMPARE(model.queryEntriesExamined(), 5);
    QVERIFY(model.listingReads() > reads);
    QCOMPARE(model.rowCount(), 4);
    model.setFilter("alpha-"); QTRY_VERIFY(!model.loading());
    QCOMPARE(model.rowCount(), 3);
    QVERIFY(QFile::rename(added.fileName(), tree.filePath("beta-new.txt")));
    model.reload(); QTRY_VERIFY(!model.loading());
    QCOMPARE(model.queryEntriesExamined(), 5);
    QCOMPARE(model.rowCount(), 2);
    const int refreshed = model.listingReads();
    m_index->refreshStorage();
    QTRY_VERIFY(!m_index->loading() && !model.loading());
    QVERIFY(model.listingReads() > refreshed);
    QCOMPARE(model.queryEntriesExamined(), 5);
    QTemporaryDir other;
    QFile otherFile(other.filePath("alpha-other.txt"));
    QVERIFY(otherFile.open(QIODevice::WriteOnly)); otherFile.close();
    state.setCwd(other.path()); QTRY_VERIFY(!model.loading());
    QCOMPARE(model.queryEntriesExamined(), 1);
    QCOMPARE(model.rowCount(), 2);
}

void TestStudio::keepsCanceledQueriesOutOfNarrowingContext()
{
    QTemporaryDir tree;
    QVERIFY(tree.isValid());
    for (const QString name : {"alpha-one.txt", "alpha-two.txt", "alpine.txt", "beta.txt"}) {
        QFile file(tree.filePath(name));
        QVERIFY(file.open(QIODevice::WriteOnly));
    }
    TabState state("files");
    state.setCwd(tree.path());
    FolderModel model;
    model.setTab(&state);
    QTRY_VERIFY(!model.loading());
    model.setFilter("alpha");
    QTRY_VERIFY(!model.loading());
    QCOMPARE(model.rowCount(), 3);
    const int reads = model.listingReads();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QTRY_VERIFY(model.findChildren<QFutureWatcherBase *>().isEmpty());
    auto *pool = QThreadPool::globalInstance();
    pool->waitForDone();
    const int threads = pool->maxThreadCount();
    pool->setMaxThreadCount(1);
    QSemaphore entered, release;
    const auto cleanup = qScopeGuard([&] {
        release.release();
        pool->waitForDone();
        pool->setMaxThreadCount(threads);
    });
    const auto blocker = QtConcurrent::run([&] { entered.release(); release.acquire(); });
    QVERIFY(entered.tryAcquire(1, asyncTimeout));
    model.setFilter("al");
    QTRY_COMPARE(model.findChildren<QFutureWatcherBase *>().size(), 1);
    model.setFilter("alp");
    QTRY_COMPARE(model.findChildren<QFutureWatcherBase *>().size(), 2);
    QSignalSpy published(&model, &FolderModel::changed);
    release.release();
    QTRY_VERIFY(!model.loading());
    pool->waitForDone();
    QCoreApplication::processEvents();
    QCOMPARE(published.size(), 1);
    QCOMPARE(model.queryEntriesExamined(), 4);
    QCOMPARE(model.listingReads(), reads);
    QStringList names;
    for (int row = 1; row < model.rowCount(); ++row)
        names.append(model.data(model.index(row), FolderModel::NameRole).toString());
    QCOMPARE(names, (QStringList{"alpha-one.txt", "alpha-two.txt", "alpine.txt"}));
    model.setFilter("alpha");
    QTRY_VERIFY(!model.loading());
    QCOMPARE(model.queryEntriesExamined(), 3);
    QCOMPARE(model.rowCount(), 3);
}

void TestStudio::narrowsUnicodeQueriesWithoutSplittingSurrogates()
{
    QTemporaryDir tree;
    QVERIFY(tree.isValid());
    const QStringList names{"K.txt", QStringLiteral("\u212aelvin.txt"),
        "s-alpha.txt", QStringLiteral("\u017fample.txt"),
        QStringLiteral("\U00010400x.txt"), QStringLiteral("\U00010428x.txt"),
        QStringLiteral("\U0001f642.txt")};
    for (const QString &name : names) {
        QFile file(tree.filePath(name));
        QVERIFY(file.open(QIODevice::WriteOnly));
    }
    TabState state("files");
    state.setCwd(tree.path());
    FolderModel model;
    model.setTab(&state);
    QTRY_VERIFY(!model.loading());
    QCOMPARE(model.directoryIndexCapacity(), 0);
    const int reads = model.listingReads();
    const QStringList queries{"k", QStringLiteral("\u212a"), QStringLiteral("\u212ae"),
        "s", QStringLiteral("\u017f"), QStringLiteral("\u017fa"),
        QString(QChar(0xd801)), QStringLiteral("\U00010400"), QStringLiteral("\U00010428"),
        QStringLiteral("\U00010428x"), QString(QChar(0xd83d)),
        QStringLiteral("\U0001f642"), QStringLiteral("\U0001f642."), ""};
    const QList<int> examined{7, 2, 2, 7, 2, 2, 7, 7, 2, 2, 7, 7, 1, 0};
    QCOMPARE(queries.size(), examined.size());
    for (int step = 0; step < queries.size(); ++step) {
        const QString &query = queries.at(step);
        model.setFilter(query);
        QTRY_VERIFY(!model.loading());
        QCOMPARE(model.queryEntriesExamined(), examined.at(step));
        QStringList expected, actual;
        for (const QString &name : names)
            if (name.contains(query, Qt::CaseInsensitive)) expected.append(name);
        QCOMPARE(model.kindAt(0), QStringLiteral("parent"));
        for (int row = 1; row < model.rowCount(); ++row) {
            QVERIFY(model.kindAt(row) != "parent");
            actual.append(model.data(model.index(row), FolderModel::NameRole).toString());
        }
        expected.sort(Qt::CaseSensitive);
        actual.sort(Qt::CaseSensitive);
        QCOMPARE(actual, expected);
        QCOMPARE(model.listingReads(), reads);
    }
    QCOMPARE(model.metadataReads(), 0);
}

void TestStudio::boundsDirectoryNameStorage()
{
    QTemporaryDir tree;
    qsizetype payload = 0;
    for (int i = 0; i < 8192; ++i) {
        const bool compact = i % 2 == 0;
        const QString name = QString(90, compact ? QChar('x') : QChar(0x3b4))
            + QString::number(i).rightJustified(6, QLatin1Char('0'));
        payload += name.size() * (compact ? 1 : 2);
        QFile file(tree.filePath(name));
        QVERIFY(file.open(QIODevice::WriteOnly));
    }
    const auto listing = DirectoryListing::read(tree.path());
    QCOMPARE(listing.size(), 8192);
    QCOMPARE(listing.entryStorageBytes(), listing.size() * 4);
    QVERIFY(listing.nameStorageCapacityBytes() >= payload);
    QVERIFY(listing.nameStorageCapacityBytes() < payload + 256 * 1024);
    for (int i = 0; i < listing.size(); ++i) {
        const QString name = listing.name(i).toString();
        QVERIFY(QFileInfo::exists(tree.filePath(name)));
        QVERIFY(listing.matches(i, DirectoryNameQuery(name), true));
    }
}

void TestStudio::reservesParentRowWithoutGrowingTheDirectoryIndex()
{
    QTemporaryDir tree;
    for (int i = 0; i < 4096; ++i) {
        QFile file(tree.filePath(QString::number(i)));
        QVERIFY(file.open(QIODevice::WriteOnly));
    }
    TabState state(QStringLiteral("files"));
    state.setCwd(tree.path());
    FolderModel model;
    model.setTab(&state);
    QTRY_VERIFY_WITH_TIMEOUT(!model.loading(), asyncTimeout);
    QCOMPARE(model.rowCount(), 4097);
    QCOMPARE(model.kindAt(0), QStringLiteral("parent"));
    QCOMPARE(model.directoryIndexCapacity(), 0);
    model.setSortAscending(false);
    QTRY_VERIFY_WITH_TIMEOUT(!model.loading(), asyncTimeout);
    QCOMPARE(model.kindAt(0), QStringLiteral("parent"));
    QVERIFY(model.directoryIndexCapacity() <= model.rowCount());
    QCOMPARE(model.data(model.index(1, 0), FolderModel::NameRole).toString(), QStringLiteral("999"));
    model.setFilter(QStringLiteral("999"));
    QTRY_VERIFY(!model.loading());
    QCOMPARE(model.rowCount(), 5);
    QCOMPARE(model.rowForPath(tree.filePath(QStringLiteral("999"))), 1);
    model.setFilter(QString());
    model.setSortAscending(true);
    QTRY_VERIFY(!model.loading());
    QCOMPARE(model.rowCount(), 4097);
    QCOMPARE(model.directoryIndexCapacity(), 0);
    QCOMPARE(model.rowForPath(tree.filePath(QStringLiteral("999"))), 4096);
    QFile hidden(tree.filePath(QStringLiteral(".hidden")));
    QVERIFY(hidden.open(QIODevice::WriteOnly));
    hidden.close();
    model.reload();
    QTRY_VERIFY(!model.loading());
    QCOMPARE(model.rowCount(), 4097);
    QCOMPARE(model.rowForPath(hidden.fileName()), -1);
    model.setShowHidden(true);
    QTRY_VERIFY(!model.loading());
    QCOMPARE(model.rowCount(), 4098);
    QCOMPARE(model.directoryIndexCapacity(), 0);
    QCOMPARE(model.rowForPath(hidden.fileName()), 1);
}

void TestStudio::reusesDirectoryNamesWhenOnlyTheIndexChanges()
{
    settle();
    TabState state(QStringLiteral("files"));
    state.setCwd(m_tree.path());
    FolderModel model;
    model.setSource(m_index);
    model.setTab(&state);
    QTRY_VERIFY(!model.loading());
    const int reads = model.listingReads();
    const int rows = model.rowCount();
    QSignalSpy published(m_index, &IndexModel::changed);
    m_index->reload();
    QTRY_VERIFY(!published.isEmpty());
    QTRY_VERIFY(!model.loading());
    QCOMPARE(model.listingReads(), reads);
    QCOMPARE(model.rowCount(), rows);

    model.reload();
    QTRY_VERIFY(!model.loading());
    QCOMPARE(model.listingReads(), reads + 1);
    QCOMPARE(model.rowCount(), rows);
}

void TestStudio::countsDirectoryScansAcrossPresencePublication_data()
{
    QTest::addColumn<bool>("presenceFirst");
    QTest::newRow("presence-before-listing") << true;
    QTest::newRow("presence-after-listing") << false;
}

void TestStudio::countsDirectoryScansAcrossPresencePublication()
{
    QFETCH(bool, presenceFirst);
    IndexModel source;
    QTRY_VERIFY(!source.loading());
    TabState state(QStringLiteral("files"));
    state.setCwd(m_tree.path());
    FolderModel model;
    model.setSource(&source);
    model.setTab(&state);
    QTRY_VERIFY(!model.loading());
    const int reads = model.listingReads();
    const int rows = model.rowCount();
    QSignalSpy published(&source, &IndexModel::changed);
    QSignalSpy storage(&source, &IndexModel::storageChanged);
    FuturePublicationBarrier presence(&source), listing(&model);
    source.refreshStorage();
    QCOMPARE(storage.size(), 1);
    QVERIFY(model.loading());
    QTRY_VERIFY(presence.worker() && listing.worker());
    QTRY_VERIFY(presence.worker()->isFinished() && listing.worker()->isFinished());
    QVERIFY(source.loading());
    QVERIFY(model.loading());
    QCOMPARE(model.listingReads(), reads + 1);
    QVERIFY(published.isEmpty());
    QSignalSpy resets(&model, &QAbstractItemModel::modelAboutToBeReset);
    presence.release();
    listing.release();
    if (presenceFirst) {
        presence.publish();
        QCOMPARE(published.size(), 1);
        listing.publish();
        QVERIFY(model.loading());
        QCOMPARE(resets.size(), 0);
    } else {
        listing.publish();
        QVERIFY(!model.loading());
        QCOMPARE(resets.size(), 1);
        presence.publish();
        QCOMPARE(published.size(), 1);
    }
    QTRY_VERIFY(!source.loading());
    QTRY_VERIFY(!model.loading());
    QCOMPARE(model.rowCount(), rows);
    QCOMPARE(model.listingReads(), reads + (presenceFirst ? 2 : 1));
}

void TestStudio::createsMenuEntriesOnlyWhileOpen()
{
    QQmlComponent component(m_engine, QUrl("qrc:/qml/ContextMenu.qml"));
    QScopedPointer<QQuickItem> unused(qobject_cast<QQuickItem *>(component.create()));
    QVERIFY2(unused, qPrintable(component.errorString()));
    QVERIFY(!unused->property("focusTarget").value<QQuickItem *>());
    QVERIFY(!unused->findChild<QQuickItem *>("overlayList"));
    QQmlComponent paletteComponent(m_engine, QUrl("qrc:/qml/CommandPalette.qml"));
    QScopedPointer<QQuickItem> unusedPalette(qobject_cast<QQuickItem *>(paletteComponent.create()));
    QVERIFY2(unusedPalette, qPrintable(paletteComponent.errorString()));
    QVERIFY(!unusedPalette->property("focusTarget").value<QQuickItem *>());
    QVERIFY(!unusedPalette->findChild<QQuickItem *>("paletteQuery"));
    settle();
    for (const QString &name : {QStringLiteral("fileMenu"), QStringLiteral("tabMenu"),
                                QStringLiteral("openWithMenu"), QStringLiteral("scriptsMenu"),
                                QStringLiteral("filterMenu"), QStringLiteral("tagMenu"),
                                QStringLiteral("deviceMenu"), QStringLiteral("viewMenu"),
                                QStringLiteral("columnMenu"), QStringLiteral("trashMenu"),
                                QStringLiteral("trashEmptyMenu"), QStringLiteral("newTabMenu")}) {
        const auto instances = itemsNamed(name);
        QVERIFY2(!instances.isEmpty(), qPrintable(name));
        for (QQuickItem *instance : instances) {
            auto *target = qvariant_cast<QQuickItem *>(instance->property("focusTarget"));
            QVERIFY(!instance->isVisible());
            if (target) QCOMPARE(target->property("count").toInt(), 0);
        }
    }
    const auto menus = itemsNamed(QStringLiteral("fileMenu"));
    QVERIFY(!menus.isEmpty());
    auto *menu = menus.first();
    QVERIFY(!menu->isVisible());
    focusRow(QStringLiteral("one.txt"));
    QVERIFY(QMetaObject::invokeMethod(menu, "openAt", Q_ARG(QVariant, 20), Q_ARG(QVariant, 20)));
    QTRY_VERIFY(menu->isVisible());
    auto *list = qvariant_cast<QQuickItem *>(menu->property("focusTarget"));
    QVERIFY(list);
    QTRY_VERIFY(list->property("count").toInt() > 0);
    QVERIFY(list->hasActiveFocus());
    key(Qt::Key_Escape);
    QTRY_VERIFY(!menu->isVisible());
    QTRY_COMPARE(list->property("count").toInt(), 0);

    auto *palette = itemsNamed(QStringLiteral("commandPalette")).first();
    QVERIFY(!palette->isVisible());
    QVERIFY(QMetaObject::invokeMethod(palette, "open"));
    QTRY_VERIFY(palette->isVisible());
    auto *commands = palette->findChild<QQuickItem *>("overlayList");
    QVERIFY(commands);
    QTRY_VERIFY(commands->property("count").toInt() > 0);
    key(Qt::Key_Escape);
    QTRY_VERIFY(!palette->isVisible());
    QTRY_COMPARE(commands->property("count").toInt(), 0);
}

void TestStudio::observesPointerMotionOncePerWindow()
{
    for (bool pen : {false, true}) {
        QQuickWindow first, second;
        auto *activity = qobject_cast<PointerActivity *>(qmlAttachedPropertiesObject<PointerActivity>(&first));
        auto *other = qobject_cast<PointerActivity *>(qmlAttachedPropertiesObject<PointerActivity>(&second));
        QVERIFY(activity);
        QVERIFY(other);
        QCOMPARE(qmlAttachedPropertiesObject<PointerActivity>(&first), activity);
        QSignalSpy changed(activity, &PointerActivity::observedChanged);
        QEnterEvent enter(QPointF(10, 10), QPointF(10, 10), QPointF(10, 10));
        QCoreApplication::sendEvent(&first, &enter);
        QKeyEvent keyboard(QEvent::KeyPress, Qt::Key_Shift, Qt::ShiftModifier);
        QCoreApplication::sendEvent(&first, &keyboard);
        QVERIFY(!activity->observed());
        QCOMPARE(changed.size(), 0);
        QPointingDevice stylus("hover pen", 43, QInputDevice::DeviceType::Stylus,
            QPointingDevice::PointerType::Pen, QInputDevice::Capability::Position, 1, 1);
        QMouseEvent mouse(QEvent::MouseMove, QPointF(10, 10), QPointF(10, 10),
            QPointF(10, 10), Qt::NoButton, Qt::NoButton, Qt::NoModifier);
        QTabletEvent tablet(QEvent::TabletMove, &stylus, QPointF(10, 10), QPointF(10, 10),
            0, 0, 0, 0, 0, 0, Qt::NoModifier, Qt::NoButton, Qt::NoButton);
        QEvent *move = pen ? static_cast<QEvent *>(&tablet) : &mouse;
        QCoreApplication::sendEvent(&first, move);
        QVERIFY(activity->observed());
        QVERIFY(!other->observed());
        QCOMPARE(changed.size(), 1);
        QCoreApplication::sendEvent(&first, move);
        QCOMPARE(changed.size(), 1);
    }
}

void TestStudio::opensTooltipsOnlyAfterPointerMotion()
{
    QQuickWindow host;
    host.setGeometry(QRect(QCursor::pos() - QPoint(10, 10), QSize(200, 100)));
    QQmlComponent component(m_engine, QUrl("qrc:/qml/FocusableControl.qml"));
    QScopedPointer<QQuickItem> control(qobject_cast<QQuickItem *>(component.createWithInitialProperties(
        {{QStringLiteral("tooltipCaption"), QStringLiteral("Hover help")},
         {QStringLiteral("width"), 50}, {QStringLiteral("height"), 50}})));
    QVERIFY2(control, qPrintable(component.errorString()));
    control->setParentItem(host.contentItem());
    host.show();
    QTRY_VERIFY(host.isExposed());
    QTRY_VERIFY(control->property("hovered").toBool());
    auto *activity = qobject_cast<PointerActivity *>(qmlAttachedPropertiesObject<PointerActivity>(&host));
    QVERIFY(activity);
    QVERIFY(!activity->observed());
    QVERIFY(!control->property("tooltipHovered").toBool());
    QVERIFY(!control->property("tooltip").value<QObject *>());
    QTest::mouseMove(&host, QPoint(12, 12));
    QTRY_VERIFY(activity->observed());
    QTRY_VERIFY(control->property("tooltipHovered").toBool());
    QTRY_VERIFY(control->property("tooltip").value<QObject *>());
    auto *tip = control->property("tooltip").value<QObject *>();
    QTRY_VERIFY(tip->property("opened").toBool());
    QTest::mouseMove(&host, QPoint(150, 80));
    QTRY_VERIFY(!tip->property("opened").toBool());
}

void TestStudio::createsSheetControlsOnlyOnDemand()
{
    for (const QString &type : {QStringLiteral("ModeSheet"), QStringLiteral("ArchiveSheet"),
                                QStringLiteral("TagSheet"), QStringLiteral("HelpSheet")}) {
        QQmlComponent component(m_engine, QUrl("qrc:/qml/" + type + ".qml"));
        QScopedPointer<QQuickItem> sheet(qobject_cast<QQuickItem *>(component.create()));
        QVERIFY2(sheet, qPrintable(component.errorString()));
        sheet->setParentItem(window()->contentItem());
        sheet->setSize(window()->size());
        auto *loader = sheet->findChild<QObject *>("sheetContentLoader");
        QVERIFY2(loader, qPrintable(type));
        QVERIFY(!loader->property("active").toBool());
        QVERIFY(!loader->property("item").value<QObject *>());
        QVERIFY(!sheet->property("focusTarget").value<QQuickItem *>());
        const auto open = [&] {
            const QVariant paths = QStringList{m_tree.filePath("one.txt")};
            if (type == QLatin1String("ModeSheet"))
                return QMetaObject::invokeMethod(sheet.data(), "openFor", Q_ARG(QVariant, paths),
                    Q_ARG(QVariant, QVariant(QVariantMap{{"permissions", 0644}, {"mine", true}})));
            if (type == QLatin1String("ArchiveSheet"))
                return QMetaObject::invokeMethod(sheet.data(), "openFor", Q_ARG(QVariant, paths),
                    Q_ARG(QVariant, false), Q_ARG(QVariant, m_tree.path()));
            if (type == QLatin1String("TagSheet"))
                return QMetaObject::invokeMethod(sheet.data(), "openFor", Q_ARG(QVariant, paths),
                    Q_ARG(QVariant, "add"), Q_ARG(QVariant, ""));
            return QMetaObject::invokeMethod(sheet.data(), "open");
        };
        QVERIFY(open());
        QVERIFY(sheet->isVisible());
        auto *content = loader->property("item").value<QObject *>();
        QVERIFY(content);
        auto *focus = sheet->property("focusTarget").value<QQuickItem *>();
        QVERIFY(focus);
        QTRY_VERIFY(focus->hasActiveFocus());
        QVERIFY(QMetaObject::invokeMethod(sheet.data(), "close"));
        QVERIFY(!sheet->isVisible());
        QCOMPARE(loader->property("item").value<QObject *>(), content);
        QVERIFY(open());
        QCOMPARE(loader->property("item").value<QObject *>(), content);
        QTRY_VERIFY(focus->hasActiveFocus());
        QVERIFY(QMetaObject::invokeMethod(sheet.data(), "close"));
    }
}

void TestStudio::suspendsHiddenOverlayFocusTracking()
{
    QQmlComponent component(m_engine, QUrl("qrc:/qml/OverlayScroll.qml"));
    QScopedPointer<QQuickItem> viewport(qobject_cast<QQuickItem *>(
        component.createWithInitialProperties({{QStringLiteral("visible"), false}})));
    QVERIFY2(viewport, qPrintable(component.errorString()));
    viewport->setParentItem(window()->contentItem());
    auto *observer = viewport->findChild<QObject *>("overlayFocusObserver");
    QVERIFY(observer);
    QVERIFY(!observer->property("enabled").toBool());
    viewport->setVisible(true);
    QVERIFY(observer->property("enabled").toBool());
    viewport->setVisible(false);
    QVERIFY(!observer->property("enabled").toBool());
}

void TestStudio::retiresDeletedControlsBeforeInspectingTheWindow()
{
    settle();
    auto *control = new QQuickItem(window()->contentItem());
    control->setObjectName(QStringLiteral("retiringControl"));
    QPointer<QQuickItem> retired(control);
    control->deleteLater();
    settle();
    QVERIFY(retired.isNull());
    QVERIFY(itemsNamed(QStringLiteral("retiringControl")).isEmpty());
}

void TestStudio::cancelsDirectoryWorkAndClearsNavigationState()
{
    settle();
    TabState state(QStringLiteral("files"));
    state.setCwd(m_tree.path());
    FolderModel model;
    model.setSource(m_index);
    model.setTab(&state);
    QTRY_VERIFY(!model.loading());
    QVERIFY(model.taggedCount() > 0);
    QThreadPool *pool = QThreadPool::globalInstance();
    pool->waitForDone();
    const int threads = pool->maxThreadCount();
    pool->setMaxThreadCount(1);
    QSemaphore entered, release;
    const auto cleanup = qScopeGuard([&] {
        release.release();
        pool->waitForDone();
        pool->setMaxThreadCount(threads);
    });
    const auto blocker = QtConcurrent::run([&] {
        entered.release();
        release.acquire();
    });
    QVERIFY(entered.tryAcquire(1, asyncTimeout));
    model.reload();
    QCoreApplication::processEvents();
    QVERIFY(model.loading());
    QTemporaryDir other;
    bool publishedOldRows = false;
    connect(&model, &FolderModel::changed, &model, [&] {
        if (model.directory() == other.path() && model.rowCount() > 1)
            publishedOldRows = true;
    });
    state.setCwd(other.path());
    QCOMPARE(model.taggedCount(), 0);
    QCOMPARE(model.rowCount(), 1);
    QCoreApplication::processEvents();
    release.release();
    QTRY_VERIFY(!model.loading());
    pool->waitForDone();
    QCoreApplication::processEvents();
    QVERIFY(!publishedOldRows);
    QCOMPARE(model.rowCount(), 1);
    state.setCwd(other.filePath("missing"));
    QTRY_VERIFY(!model.loading());
    QVERIFY(!model.error().isEmpty());
    state.setCwd(other.path());
    QVERIFY(model.error().isEmpty());
    QTRY_VERIFY(!model.loading());

    focusRow(QStringLiteral("one.txt"));
    QVERIFY(!filesRoot()->property("entry").toMap().isEmpty());
    tab()->setCwd(other.path());
    QVERIFY(filesRoot()->property("entry").toMap().isEmpty());
    settle();
    tab()->setCwd(m_tree.path());
    settle();
}

void TestStudio::keepsEveryKeyboardMoveInTheViewport()
{
    QTemporaryDir tree;
    for (int i = 0; i < 1024; ++i) {
        QFile file(tree.filePath(QStringLiteral("row-%1.txt").arg(i, 6, 10, QLatin1Char('0'))));
        QVERIFY(file.open(QIODevice::WriteOnly));
    }
    tab()->setCwd(tree.path());
    settle();
    for (const QString &mode : {QStringLiteral("list"), QStringLiteral("details"), QStringLiteral("icons")}) {
        tab()->setViewMode(mode);
        settle();
        QObject *viewObject = filesRoot()->property("view").value<QObject *>();
        auto *view = qobject_cast<QQuickItem *>(viewObject);
        QVERIFY(view);
        const auto visibleCursor = [&] {
            auto *item = qobject_cast<QQuickItem *>(viewObject->property("currentItem").value<QObject *>());
            if (!item)
                return false;
            const QPointF position = item->mapToItem(view, QPointF());
            return position.y() >= -1 && position.y() + item->height() <= view->height() + 1;
        };
        for (Qt::Key keyCode : {Qt::Key_End, Qt::Key_PageUp, Qt::Key_PageDown, Qt::Key_Home,
                           Qt::Key_Down, Qt::Key_Up}) {
            key(keyCode);
            QVERIFY2(visibleCursor(), qPrintable(mode));
        }
        key(Qt::Key_End);
        QCOMPARE(cursor(), 1024);
        QVERIFY(visibleCursor());
        key(Qt::Key_Home);
        QVERIFY(visibleCursor());
        QCOMPARE(viewObject->property("highlightMoveDuration").toInt(), 0);
        if (mode != QLatin1String("icons"))
            QCOMPARE(viewObject->property("highlightResizeDuration").toInt(), 0);
        const QPointF point = view->mapToScene(QPointF(view->width() / 2, view->height() / 2));
        QWheelEvent wheel(point, window()->mapToGlobal(point.toPoint()), QPoint(), QPoint(0, -120000000),
                          Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
        QCoreApplication::sendEvent(window(), &wheel);
        QVERIFY(!viewObject->property("flicking").toBool());
        const double end = viewObject->property("originY").toDouble()
                         + viewObject->property("contentHeight").toDouble() - view->height();
        QVERIFY(viewObject->property("contentY").toDouble() >= end - 1);
        key(Qt::Key_Home);
        QVERIFY(visibleCursor());
    }
    tab()->setCwd(m_tree.path());
    tab()->setViewMode("list");
    settle();
}

QTEST_MAIN(TestStudio)
// A search path entry that is not UTF-8 is skipped, the way the basedir spec
// ignores an invalid one: decoded, `\xff` names the U+FFFD directory beside it.
void TestStudio::searchesOnlyTheDirectoriesItCanSpell()
{
    QTemporaryDir desk;
    QVERIFY(desk.isValid());
    const QDir base(desk.path());
    const QString shadow = base.filePath(QStringLiteral("\uFFFD"));
    const auto write = [](const QString &path, const QByteArray &body) {
        QDir().mkpath(QFileInfo(path).absolutePath());
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
            return false;
        return file.write(body) == body.size();
    };
    const auto entry = [](const char *name) {
        return QByteArray("[Desktop Entry]\nType=Application\nName=") + name
            + "\nExec=x %f\n";
    };
    const QString shadowApps = QDir(shadow).filePath(QStringLiteral("applications"));
    const QString keptApps = base.filePath(QStringLiteral("kept/applications"));
    const QString homeApps = QDir(m_home.path()).filePath(QStringLiteral("applications"));
    QVERIFY(write(QDir(shadowApps).filePath(QStringLiteral("mimeinfo.cache")),
                  "[MIME Cache]\ntext/plain=oma-shadow.desktop;\n"));
    QVERIFY(write(QDir(shadowApps).filePath(QStringLiteral("oma-shadow.desktop")),
                  entry("Oma Shadow")));
    QVERIFY(write(QDir(shadow).filePath(QStringLiteral("mimeapps.list")),
                  "[Default Applications]\ntext/plain=oma-elsewhere.desktop;\n"));
    QVERIFY(write(QDir(homeApps).filePath(QStringLiteral("oma-elsewhere.desktop")),
                  entry("Oma Elsewhere")));
    QVERIFY(write(QDir(keptApps).filePath(QStringLiteral("mimeinfo.cache")),
                  "[MIME Cache]\ntext/plain=oma-kept.desktop;\n"));
    QVERIFY(write(QDir(keptApps).filePath(QStringLiteral("oma-kept.desktop")),
                  entry("Oma Kept")));

    const QByteArray unspelled = QFile::encodeName(desk.path()) + "/\xff";
    const QByteArray dataDirs = qgetenv("XDG_DATA_DIRS");
    const QByteArray configDirs = qgetenv("XDG_CONFIG_DIRS");
    const auto restore = qScopeGuard([&] {
        qputenv("XDG_DATA_DIRS", dataDirs);
        qputenv("XDG_CONFIG_DIRS", configDirs);
        QDir(homeApps).removeRecursively();
    });
    // The system's own directories stay on the end: the type of a file is
    // looked up in the MIME database under them.
    qputenv("XDG_DATA_DIRS", unspelled + ':' + QFile::encodeName(base.filePath("kept")) + ':'
                                 + (dataDirs.isEmpty() ? "/usr/local/share:/usr/share" : dataDirs));
    qputenv("XDG_CONFIG_DIRS", unspelled + ':' + (configDirs.isEmpty() ? "/etc/xdg" : configDirs));

    QStringList names;
    for (const QVariant &app :
         m_index->applicationsFor(QDir(m_tree.path()).filePath(QStringLiteral("one.txt"))))
        names.append(app.toMap().value(QStringLiteral("name")).toString());
    QVERIFY2(names.contains(QStringLiteral("Oma Kept")), qPrintable(names.join(u'|')));
    QVERIFY2(!names.contains(QStringLiteral("Oma Shadow")), qPrintable(names.join(u'|')));
    QVERIFY2(!names.contains(QStringLiteral("Oma Elsewhere")), qPrintable(names.join(u'|')));
}

// A file where a row's directory was means nothing can be at the path, so the
// window counts the row gone and names `prune`, which drops it.
void TestStudio::countsARowUnderAFileAsGone()
{
    QTemporaryDir fresh;
    QVERIFY(fresh.isValid());
    const QDir data(fresh.path());
    QVERIFY(data.mkpath(QStringLiteral("omafiles")));
    QFile plain(data.filePath(QStringLiteral("plain")));
    QVERIFY(plain.open(QIODevice::WriteOnly));
    plain.close();
    QFile index(data.filePath(QStringLiteral("omafiles/index.jsonl")));
    QVERIFY(index.open(QIODevice::WriteOnly | QIODevice::Text));
    index.write(QStringLiteral("{\"path\":\"%1/inside\",\"tags\":[\"x\"]}\n")
                    .arg(plain.fileName())
                    .toUtf8());
    index.close();
    const QByteArray previous = qgetenv("XDG_DATA_HOME");
    qputenv("XDG_DATA_HOME", fresh.path().toUtf8());
    const auto restore = qScopeGuard([&previous] { qputenv("XDG_DATA_HOME", previous); });

    IndexModel model;
    QTRY_VERIFY(!model.loading());
    QVERIFY2(model.error().contains(QStringLiteral("none of their files are there")),
             qPrintable(model.error()));
}

// The window opens through the plan alone, so a stray file in the mount point
// of a root that is away is refused there.
void TestStudio::plansNothingUnderARootThatIsAway()
{
    QTemporaryDir fresh;
    QVERIFY(fresh.isValid());
    const QDir room(fresh.path());
    QFile stray(room.filePath(QStringLiteral("stray.txt")));
    QVERIFY(stray.open(QIODevice::WriteOnly));
    stray.close();
    const QVector<Root> previousRoots = declareRoots(
        {Root{QStringLiteral("disk"), room.absolutePath(), false, QStringLiteral(".here")}});
    const auto restore = qScopeGuard([&previousRoots] { declareRoots(previousRoots); });

    OpenPlan plan;
    QString why;
    PathState state = PathState::Present;
    QVERIFY2(!planOpen(stray.fileName(), &plan, &why, &state), "a stray file was planned");
    QCOMPARE(state, PathState::Unreachable);
    QVERIFY2(why.contains(QStringLiteral("root disk is declared and not here")), qPrintable(why));
}

// A row counts where resolving its path gives it back, so a link now at a
// row's path, or at a directory above it, is untagged in the window as in `ls`.
void TestStudio::readsNoTagsAtALinkWhereARowWas()
{
    QTemporaryDir fresh;
    QVERIFY(fresh.isValid());
    const QDir room(fresh.path());
    QVERIFY(room.mkpath(QStringLiteral("real")));
    for (const QString &name : {QStringLiteral("q"), QStringLiteral("real/f")}) {
        QFile file(room.filePath(name));
        QVERIFY(file.open(QIODevice::WriteOnly));
    }
    QVERIFY(QFile::link(room.filePath(QStringLiteral("q")), room.filePath(QStringLiteral("p"))));
    QVERIFY(QFile::link(room.filePath(QStringLiteral("real")),
                        room.filePath(QStringLiteral("dir"))));
    QVERIFY(room.mkpath(QStringLiteral("data/omafiles")));
    QFile index(room.filePath(QStringLiteral("data/omafiles/index.jsonl")));
    QVERIFY(index.open(QIODevice::WriteOnly | QIODevice::Text));
    for (const QString &name : {QStringLiteral("dir/f"), QStringLiteral("p")})
        index.write(QStringLiteral("{\"path\":\"%1\",\"tags\":[\"t\"]}\n")
                        .arg(room.filePath(name))
                        .toUtf8());
    index.close();
    const QByteArray previous = qgetenv("XDG_DATA_HOME");
    qputenv("XDG_DATA_HOME", room.filePath(QStringLiteral("data")).toUtf8());
    const auto restore = qScopeGuard([&previous] { qputenv("XDG_DATA_HOME", previous); });

    IndexModel model;
    QTRY_VERIFY(!model.loading());
    QCOMPARE(model.tagsForPath(room.filePath(QStringLiteral("p"))), QStringList());
    QCOMPARE(model.tagsForPath(room.filePath(QStringLiteral("dir/f"))), QStringList());
}

// A file re-cased on a casefold tmpfs since the sweep keeps its row in a listing;
// the run repeats itself in a user and mount namespace, or skips without one.
void TestStudio::listsARecasedNameUnderItsRow()
{
    const QString folded = qEnvironmentVariable("OMAFILES_TEST_CASEFOLD");
    if (folded.isEmpty()) {
        const QString unshare = QStandardPaths::findExecutable(QStringLiteral("unshare"));
        if (unshare.isEmpty())
            QSKIP("no unshare here");
        QTemporaryDir cf;
        QVERIFY(cf.isValid());
        QProcess inner;
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert(QStringLiteral("OMAFILES_TEST_CASEFOLD"), cf.path());
        inner.setProcessEnvironment(env);
        inner.start(unshare,
                    {QStringLiteral("-rm"), QStringLiteral("sh"), QStringLiteral("-c"),
                     QStringLiteral("mount -t tmpfs -o casefold tmpfs \"$2\" && chattr +F \"$2\" "
                                    "|| exit 97; exec \"$3\" -U --map-user=%1 \"$0\" \"$1\"")
                         .arg(::getuid()),
                     QCoreApplication::applicationFilePath(),
                     QString::fromLatin1(QTest::currentTestFunction()), cf.path(), unshare});
        QVERIFY(inner.waitForFinished(60000));
        const QString said = QString::fromUtf8(inner.readAll());
        if (inner.exitCode() == 97)
            QSKIP("no casefold tmpfs in a namespace here");
        QVERIFY2(inner.exitCode() == 0
                     && said.contains(QStringLiteral("PASS   : TestStudio::listsARecasedNameUnderItsRow")),
                 qPrintable(said));
        return;
    }

    QTemporaryDir cache;
    QVERIFY(cache.isValid());
    const QDir room(folded);
    for (const char *name : {"foo.txt", "other.txt"}) {
        QFile file(room.filePath(QString::fromLatin1(name)));
        QVERIFY(file.open(QIODevice::WriteOnly));
    }
    qputenv("XDG_CACHE_HOME", cache.path().toUtf8());
    declareRoots({Root{QStringLiteral("usb"), room.absolutePath(), false, QString()}});
    QVERIFY(writeNames(QStringLiteral("usb"),
                       {QStringLiteral("foo.txt"), QStringLiteral("other.txt")}, {}, nullptr,
                       nullptr));
    tagDirectory(room.filePath(QStringLiteral("foo.txt")), QStringLiteral("usb"), false);
    QVERIFY(QFile::rename(room.filePath(QStringLiteral("foo.txt")),
                          room.filePath(QStringLiteral("t1"))));
    QVERIFY(QFile::rename(room.filePath(QStringLiteral("t1")),
                          room.filePath(QStringLiteral("Foo.TXT"))));
    m_index->reload();
    settle();

    QObject *folder = filesRoot()->property("folder").value<QObject *>();
    QVERIFY(folder);
    const auto files = [&folder] {
        QStringList out;
        for (int i = 0; i < folder->property("count").toInt(); ++i) {
            QVariantMap entry;
            QMetaObject::invokeMethod(folder, "entryAt", Q_RETURN_ARG(QVariantMap, entry),
                                      Q_ARG(int, i));
            if (entry.value(QStringLiteral("kind")).toString() == QLatin1String("file"))
                out.append(entry.value(QStringLiteral("name")).toString());
        }
        return out;
    };
    key('f');
    QMetaObject::invokeMethod(m_engine->rootObjects().first(), "finishFinding",
                              Q_ARG(QVariant, QVariant(true)),
                              Q_ARG(QVariant, QVariant(QStringLiteral("f*"))));
    settle();
    QCOMPARE(files(), QStringList{QStringLiteral("Foo.TXT")});

    m_workspace->addTab(QStringLiteral("tags"));
    settle();
    folder = filesRoot()->property("folder").value<QObject *>();
    tab()->setUntagged(true);
    settle();
    QCOMPARE(files(), QStringList{QStringLiteral("other.txt")});
}


void TestStudio::resolvesDesktopLocations()
{
    QList<OpenLocation> locations;
    QString error;
    const QString folder = m_tree.filePath("alpha");
    const QString file = m_tree.filePath("two.md");
    const QString uri = QUrl::fromLocalFile(file).toString();
    QVERIFY(FileManager::resolve({folder, uri}, FileManager::Automatic, &locations, &error));
    QCOMPARE(locations.size(), 2);
    QCOMPARE(locations[0].directory, folder);
    QVERIFY(locations[0].cursor.isEmpty());
    QCOMPARE(locations[1].directory, m_tree.path());
    QCOMPARE(locations[1].cursor, file);
    Workspace initial;
    FileManager startup;
    startup.setWorkspace(&initial);
    startup.open(locations, true);
    QCOMPARE(initial.paneAtIndex(0)->tabCount(), 2);
    QCOMPARE(initial.tabAtIndex(0, 0)->cwd(), folder);
    QCOMPARE(initial.currentTab()->cursorPath(), file);
    QVERIFY(FileManager::resolve({folder}, FileManager::Items, &locations, &error));
    QCOMPARE(locations[0].directory, m_tree.path());
    QCOMPARE(locations[0].cursor, folder);
    for (const QString &bad : {QStringLiteral("file:///tmp/%ff"),
                              QStringLiteral("file:///tmp/%GG"),
                              QStringLiteral("file://remote/tmp"),
                              QStringLiteral("https://example.org/"),
                              uri + QStringLiteral("#fragment"),
                              m_tree.filePath("missing")}) {
        QVERIFY2(!FileManager::resolve({folder, bad}, FileManager::Automatic, &locations, &error),
                 qPrintable(bad));
        QVERIFY(!error.isEmpty());
    }
    QVERIFY(!FileManager::resolve({file}, FileManager::Folders, &locations, &error));
    QTemporaryDir encoded;
    const QString unusual = encoded.filePath("colon: space # % file");
    QFile named(unusual);
    QVERIFY(named.open(QIODevice::WriteOnly));
    named.close();
    QVERIFY(FileManager::resolve({unusual, QUrl::fromLocalFile(unusual).toString(QUrl::FullyEncoded)},
                                 FileManager::Automatic, &locations, &error));
    QCOMPARE(locations.size(), 2);
    QCOMPARE(locations[0].cursor, unusual);
    QCOMPARE(locations[1].cursor, unusual);
    QFile malformed(encoded.filePath("%GG"));
    QVERIFY(malformed.open(QIODevice::WriteOnly));
    malformed.close();
    const QString malformedUri = QStringLiteral("file://") + encoded.path() + "/%GG";
    QVERIFY(!FileManager::resolve({malformedUri}, FileManager::Automatic, &locations, &error));

    const QString link = m_tree.filePath("desktop-link");
    QVERIFY(QFile::link(file, link));
    QVERIFY(FileManager::resolve({link}, FileManager::Items, &locations, &error));
    QCOMPARE(locations[0].cursor, link);
    QVERIFY(QFile::remove(link));
}

void TestStudio::revealsDesktopItemsAndProperties()
{
    FileManager manager;
    manager.setWorkspace(m_workspace);
    const QString file = m_tree.filePath("sweep/.hidden.txt");
    const QString uri = QUrl::fromLocalFile(file).toString();
    const int before = m_workspace->paneAtIndex(0)->tabCount();
    QSignalSpy presented(m_workspace, &Workspace::presentationRequested);
    QCOMPARE(manager.Open({m_tree.filePath("alpha"), uri}, "token"), QString());
    settle();
    QCOMPARE(m_workspace->paneAtIndex(0)->tabCount(), before + 2);
    QCOMPARE(cwd(), m_tree.filePath("sweep"));
    QVERIFY(tab()->showHidden());
    QCOMPARE(tab()->cursorPath(), file);
    QCOMPARE(filesRoot()->property("entry").toMap().value("path").toString(), file);
    QCOMPARE(presented.last().at(0).toString(), QStringLiteral("token"));
    manager.ShowItems({QUrl::fromLocalFile(m_tree.filePath("alpha")).toString()}, "");
    settle();
    QCOMPARE(cwd(), m_tree.path());
    QCOMPARE(tab()->cursorPath(), m_tree.filePath("alpha"));
    manager.ShowFolders({QUrl::fromLocalFile(m_tree.filePath("beta")).toString()}, "");
    settle();
    QCOMPARE(cwd(), m_tree.filePath("beta"));
    QVERIFY(tab()->cursorPath().isEmpty() || tab()->cursorPath().startsWith(cwd() + '/'));
    manager.ShowItemProperties({uri}, "");
    settle();
    QPointer<QObject> dialog = m_engine->rootObjects().first()->findChild<QObject *>("propertiesDialog");
    QVERIFY(dialog);
    QVERIFY(dialog->property("visible").toBool());
    QObject *text = dialog->findChild<QObject *>("propertyText");
    QVERIFY(text);
    QVERIFY(text->property("text").toString().contains(file));
    QVERIFY(text->property("text").toString().contains("Size: 1 bytes"));
    QVERIFY(text->property("text").toString().contains("Permissions: -rw"));
    QMetaObject::invokeMethod(dialog, "close");
    while (m_workspace->paneAtIndex(0)->tabCount() > before)
        m_workspace->closeCurrentTab();
    settle();
}

void TestStudio::exportsFileManagerOnAPrivateBus()
{
    QTemporaryDir runtime;
    QProcess daemon;
    auto daemonEnvironment = QProcessEnvironment::systemEnvironment();
    daemonEnvironment.insert("XDG_RUNTIME_DIR", runtime.path());
    daemon.setProcessEnvironment(daemonEnvironment);
    daemon.start(QStringLiteral("dbus-daemon"), {"--session", "--nofork", "--print-address=1"});
    QVERIFY(daemon.waitForStarted());
    const auto cleanup = qScopeGuard([&] { daemon.terminate(); daemon.waitForFinished(); });
    QVERIFY2(daemon.bytesAvailable() || daemon.waitForReadyRead(), daemon.readAllStandardError().constData());
    const QString address = QString::fromUtf8(daemon.readLine()).trimmed();
    auto server = QDBusConnection::connectToBus(address, "t01-server");
    auto client = QDBusConnection::connectToBus(address, "t01-client");
    const auto disconnect = qScopeGuard([] {
        QDBusConnection::disconnectFromBus("t01-client");
        QDBusConnection::disconnectFromBus("t01-server");
    });
    FileManager manager;
    manager.setWorkspace(m_workspace);
    const QString path = QStringLiteral("/org/freedesktop/FileManager1");
    const QString interface = QStringLiteral("org.freedesktop.FileManager1");
    QVERIFY(server.registerObject(path, &manager, QDBusConnection::ExportAllSlots));
    QVERIFY(server.registerService(interface));
    auto call = [&](const QString &method, const QStringList &uris) {
        QDBusMessage message = QDBusMessage::createMethodCall(interface, path, interface, method);
        message << uris << QString();
        QDBusPendingCallWatcher pending(client.asyncCall(message));
        QSignalSpy finished(&pending, &QDBusPendingCallWatcher::finished);
        if (!pending.isFinished())
            finished.wait(asyncTimeout);
        return pending.reply();
    };
    const QString uri = QUrl::fromLocalFile(m_tree.filePath("beta")).toString();
    QCOMPARE(call("ShowFolders", {uri}).type(), QDBusMessage::ReplyMessage);
    QCOMPARE(cwd(), m_tree.filePath("beta"));
    QCOMPARE(call("ShowItems", {uri}).type(), QDBusMessage::ReplyMessage);
    QCOMPARE(tab()->cursorPath(), m_tree.filePath("beta"));
    QSignalSpy properties(m_workspace, &Workspace::propertiesRequested);
    QCOMPARE(call("ShowItemProperties", {uri}).type(), QDBusMessage::ReplyMessage);
    QCOMPARE(properties.count(), 1);
    QCOMPARE(call("ShowFolders", {"file:///missing-t01"}).type(), QDBusMessage::ErrorMessage);
    QPointer<QObject> dialog = m_engine->rootObjects().first()->findChild<QObject *>("propertiesDialog");
    QMetaObject::invokeMethod(dialog, "close");
    m_workspace->closeCurrentTab();
    m_workspace->closeCurrentTab();
}


void TestStudio::makesTheFileManagerDefaultFromTheKeyboard()
{
    QTemporaryDir isolated;
    const QByteArray data = qgetenv("XDG_DATA_HOME");
    const QByteArray config = qgetenv("XDG_CONFIG_HOME");
    const auto restore = qScopeGuard([&] { qputenv("XDG_DATA_HOME", data); qputenv("XDG_CONFIG_HOME", config); });
    qputenv("XDG_DATA_HOME", isolated.filePath("data with spaces").toUtf8());
    qputenv("XDG_CONFIG_HOME", isolated.filePath("config").toUtf8());
    const QString service = dataHome() + "/dbus-1/services/org.freedesktop.FileManager1.service";
    QVERIFY(QDir().mkpath(typedDirectory(service, nullptr)));
    QFile previous(service);
    QVERIFY(previous.open(QIODevice::WriteOnly));
    previous.write("[D-BUS Service]\nName=org.freedesktop.FileManager1\nExec=/other/manager\n");
    previous.close();
    const auto read = [](const QString &path) {
        QFile file(path);
        return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
    };
    const QByteArray original = read(service);
    openRequests().clear();
    QSemaphore guiAnswered;
    Launcher originalLauncher = setLauncher([&](const OpenRequest &request, QString *error) {
        if (!guiAnswered.tryAcquire(1, asyncTimeout)) {
            *error = QStringLiteral("the GUI was blocked by default selection");
            return false;
        }
        guiAnswered.release();
        return originalLauncher(request, error);
    });
    const auto restoreLauncher = qScopeGuard([&] { setLauncher(originalLauncher); });
    QSignalSpy finished(m_index, &IndexModel::defaultManagerFinished);
    key('p', Qt::ControlModifier);
    for (char ch : QByteArray("make omafiles"))
        key(ch);
    QTimer::singleShot(0, m_index, [&] { guiAnswered.release(); });
    key(Qt::Key_Return);
    QTRY_COMPARE(finished.count(), 1);
    QVERIFY(finished.first().first().toBool());
    QCOMPARE(openRequests().size(), 1);
    QCOMPARE(openRequests().last().kind, OpenRequest::SetDirectoryDefault);
    QCOMPARE(openRequests().last().arguments,
             (QStringList{"default", "omafiles.desktop", "inode/directory"}));
    QVERIFY(read(dataHome() + "/applications/omafiles.desktop").contains("MimeType=inode/directory;"));
    QVERIFY(read(service).contains(QCoreApplication::applicationFilePath().toUtf8()));
    QCOMPARE(read(service + ".omafiles-backup"), original);
    const QString receipt = finished.first().at(1).toString();
    QVERIFY(receipt.contains("Undo: mv --"));
    QVERIFY(receipt.contains("xdg-mime default OTHER.desktop inode/directory"));
    QVERIFY(receipt.contains(shellQuoted(service)));
    QPointer<QObject> dialog = m_engine->rootObjects().first()->findChild<QObject *>("propertiesDialog");
    QVERIFY(dialog->property("visible").toBool());
    QObject *background = dialog->property("background").value<QObject *>();
    QObject *text = dialog->findChild<QObject *>("propertyText");
    QVERIFY(background);
    QVERIFY(text);
    const auto luminance = [](QColor color) {
        const auto linear = [](double value) {
            return value <= 0.04045 ? value / 12.92 : std::pow((value + 0.055) / 1.055, 2.4);
        };
        return 0.2126 * linear(color.redF()) + 0.7152 * linear(color.greenF())
             + 0.0722 * linear(color.blueF());
    };
    const double paper = luminance(background->property("color").value<QColor>());
    const double ink = luminance(text->property("color").value<QColor>());
    QVERIFY((qMax(paper, ink) + 0.05) / (qMin(paper, ink) + 0.05) >= 4.5);
    key(Qt::Key_Escape);
    settle();
    QVERIFY(!dialog || !dialog->property("visible").toBool());
    QCOMPARE(m_engine->rootObjects().first()->property("focusRegion").toString(), QStringLiteral("list"));
    const auto again = omafiles::makeDefaultFileManager(QCoreApplication::applicationFilePath());
    QVERIFY(again.ok);
    QCOMPARE(read(service + ".omafiles-backup"), original);
    QVERIFY(again.text.contains("Already installed:"));
    const auto moved = omafiles::makeDefaultFileManager(QCoreApplication::applicationFilePath() + "-other");
    QVERIFY(moved.ok);
    QCOMPARE(read(service + ".omafiles-backup"), original);
}

void TestStudio::reportsDefaultRegistrationFailures()
{
    QTemporaryDir isolated;
    const QByteArray data = qgetenv("XDG_DATA_HOME");
    const QByteArray config = qgetenv("XDG_CONFIG_HOME");
    const auto restoreEnvironment = qScopeGuard([&] { qputenv("XDG_DATA_HOME", data); qputenv("XDG_CONFIG_HOME", config); });
    qputenv("XDG_DATA_HOME", isolated.filePath("data").toUtf8());
    qputenv("XDG_CONFIG_HOME", isolated.filePath("config").toUtf8());
    Launcher launcher = setLauncher([](const OpenRequest &, QString *error) {
        *error = QStringLiteral("simulated MIME failure");
        return false;
    });
    const auto restoreLauncher = qScopeGuard([&] { setLauncher(launcher); });
    auto result = omafiles::makeDefaultFileManager("/tmp/studio");
    QVERIFY(!result.ok);
    QVERIFY(result.text.contains("simulated MIME failure"));
    QVERIFY(result.text.contains("remain installed"));
    QFile desktop(dataHome() + "/applications/omafiles.desktop");
    QVERIFY(desktop.open(QIODevice::ReadOnly));
    QVERIFY(desktop.readAll().contains("Exec=/tmp/studio %U"));
    QFile blocked(isolated.filePath("blocked"));
    QVERIFY(blocked.open(QIODevice::WriteOnly));
    blocked.close();
    qputenv("XDG_DATA_HOME", blocked.fileName().toUtf8());
    result = omafiles::makeDefaultFileManager("/tmp/studio");
    QVERIFY(!result.ok);
    QVERIFY(result.text.contains("Cannot prepare"));
}

void TestStudio::refusesLinkedDefaultRegistrationPaths()
{
    const QByteArray oldData = qgetenv("XDG_DATA_HOME");
    const QByteArray oldConfig = qgetenv("XDG_CONFIG_HOME");
    const auto restore = qScopeGuard([&] {
        qputenv("XDG_DATA_HOME", oldData);
        qputenv("XDG_CONFIG_HOME", oldConfig);
    });
    const QString desktop = QStringLiteral("data/applications/omafiles.desktop");
    const QString service = QStringLiteral("data/dbus-1/services/org.freedesktop.FileManager1.service");
    const QStringList files = {desktop, service, desktop + ".omafiles-backup", service + ".omafiles-backup",
        "config/mimeapps.list", "config/mimeapps.list.new",
        "data/applications/mimeapps.list", "data/applications/mimeapps.list.new"};
    const QStringList directories = {"data", "data/applications", "data/dbus-1", "data/dbus-1/services", "config"};
    const auto write = [](const QString &path, const QByteArray &bytes) {
        QDir().mkpath(typedDirectory(path, nullptr));
        QFile file(path);
        return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
    };
    const auto read = [](const QString &path) {
        QFile file(path);
        return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
    };
    for (bool dangling : {false, true}) {
        for (const QString &relative : files + directories) {
            QTemporaryDir tree;
            qputenv("XDG_DATA_HOME", tree.filePath("data").toUtf8());
            qputenv("XDG_CONFIG_HOME", tree.filePath("config").toUtf8());
            QVERIFY(write(tree.filePath(desktop), "original desktop"));
            QVERIFY(write(tree.filePath(service), "original service"));
            const QString path = tree.filePath(relative);
            const QString target = tree.filePath("sentinel");
            if (directories.contains(relative)) {
                QVERIFY(QDir().mkpath(path));
                QVERIFY(QDir().rename(path, target));
                if (dangling)
                    QVERIFY(QDir(target).removeRecursively());
            } else {
                QFile::remove(path);
                if (!dangling)
                    QVERIFY(write(target, "sentinel contents"));
            }
            QVERIFY(QDir().mkpath(typedDirectory(path, nullptr)));
            QVERIFY(QFile::link(target, path));
            const QByteArray link = linkTarget(QFile::encodeName(path));
            const QByteArray originalDesktop = read(tree.filePath(desktop));
            const QByteArray originalService = read(tree.filePath(service));
            openRequests().clear();
            const auto result = makeDefaultFileManager(QCoreApplication::applicationFilePath());
            QVERIFY2(!result.ok, qPrintable(relative));
            QVERIFY2(result.text.contains("symbolic link"), qPrintable(result.text));
            QVERIFY(openRequests().isEmpty());
            QCOMPARE(linkTarget(QFile::encodeName(path)), link);
            QCOMPARE(read(tree.filePath(desktop)), originalDesktop);
            QCOMPARE(read(tree.filePath(service)), originalService);
            if (!directories.contains(relative)) {
                QCOMPARE(read(target), dangling ? QByteArray() : QByteArray("sentinel contents"));
                QCOMPARE(QFileInfo::exists(target), !dangling);
            }
        }
    }
    for (const QString &relative : {QStringLiteral("config/mimeapps.list.new"),
                                    QStringLiteral("data/applications/mimeapps.list.new")}) {
        QTemporaryDir tree;
        qputenv("XDG_DATA_HOME", tree.filePath("data").toUtf8());
        qputenv("XDG_CONFIG_HOME", tree.filePath("config").toUtf8());
        const QString path = tree.filePath(relative);
        QVERIFY(write(path, "unrelated contents"));
        const auto result = makeDefaultFileManager(QCoreApplication::applicationFilePath());
        QVERIFY(!result.ok);
        QVERIFY(result.text.contains("temporary path already exists"));
        QCOMPARE(read(path), QByteArray("unrelated contents"));
    }
    for (const QString &relative : files) {
        QTemporaryDir tree;
        qputenv("XDG_DATA_HOME", tree.filePath("data").toUtf8());
        qputenv("XDG_CONFIG_HOME", tree.filePath("config").toUtf8());
        const QString path = tree.filePath(relative);
        QVERIFY(QDir().mkpath(typedDirectory(path, nullptr)));
        QVERIFY(::mkfifo(QFile::encodeName(path).constData(), 0600) == 0);
        const auto result = makeDefaultFileManager(QCoreApplication::applicationFilePath());
        QVERIFY(!result.ok);
        QVERIFY(result.text.contains("not a regular file"));
    }
}

void TestStudio::restoresDefaultRegistrationsWithTheirModes()
{
    QTemporaryDir tree;
    const QByteArray data = qgetenv("XDG_DATA_HOME"), config = qgetenv("XDG_CONFIG_HOME");
    const mode_t mask = ::umask(0077);
    const auto restore = qScopeGuard([&] {
        ::umask(mask);
        qputenv("XDG_DATA_HOME", data);
        qputenv("XDG_CONFIG_HOME", config);
    });
    qputenv("XDG_DATA_HOME", tree.filePath("data").toUtf8());
    qputenv("XDG_CONFIG_HOME", tree.filePath("config").toUtf8());
    const QStringList paths = {dataHome() + "/applications/omafiles.desktop",
        dataHome() + "/dbus-1/services/org.freedesktop.FileManager1.service"};
    for (const QString &path : paths) {
        QVERIFY(QDir().mkpath(typedDirectory(path, nullptr)));
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write("original registration"), 21);
        file.close();
        QVERIFY(::chmod(QFile::encodeName(path).constData(), 0640) == 0);
    }
    const auto result = makeDefaultFileManager(QCoreApplication::applicationFilePath());
    QVERIFY2(result.ok, qPrintable(result.text));
    QVERIFY(makeDefaultFileManager(QCoreApplication::applicationFilePath() + "-changed").ok);
    for (const QString &path : paths) {
        const QString backup = path + ".omafiles-backup";
        QVERIFY(result.text.contains(QStringLiteral("Undo: mv -- %1 %2").arg(shellQuoted(backup), shellQuoted(path))));
        QVERIFY(::rename(QFile::encodeName(backup).constData(), QFile::encodeName(path).constData()) == 0);
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), QByteArray("original registration"));
        struct stat st {};
        QVERIFY(::lstat(QFile::encodeName(path).constData(), &st) == 0);
        QVERIFY(S_ISREG(st.st_mode));
        QCOMPARE(st.st_mode & 0777, mode_t(0640));
    }
}

void TestStudio::recognizesGeneratedDefaultEntries()
{
    const QByteArray data = qgetenv("XDG_DATA_HOME"), config = qgetenv("XDG_CONFIG_HOME");
    const auto restore = qScopeGuard([&] {
        qputenv("XDG_DATA_HOME", data);
        qputenv("XDG_CONFIG_HOME", config);
    });
    for (const QString &name : {QStringLiteral("studio"), QStringLiteral("studio-safe_1.2+3:@!,[]{}^"),
             QStringLiteral("studio-\u03bb")}) {
        QTemporaryDir tree;
        qputenv("XDG_DATA_HOME", tree.filePath("data").toUtf8());
        qputenv("XDG_CONFIG_HOME", tree.filePath("config").toUtf8());
        const QString executable = tree.filePath(name);
        QFile file(executable);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QVERIFY(file.write("#!/bin/sh\nprintf '%s\\0' \"$@\" > \"$LAUNCH_RECEIPT\"\n") > 0);
        file.close();
        QVERIFY(file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
        const auto result = makeDefaultFileManager(executable);
        QVERIFY2(result.ok, qPrintable(result.text));
        QFile fallback(dataHome() + "/applications/defaults.list");
        QVERIFY(fallback.open(QIODevice::WriteOnly));
        QVERIFY(fallback.write("[Default Applications]\ninode/directory=fallback.desktop;\n") > 0);
        fallback.close();
        QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
        for (const QString &variable : {QStringLiteral("HOME"), QStringLiteral("XDG_CACHE_HOME"),
             QStringLiteral("XDG_STATE_HOME"), QStringLiteral("TMPDIR"), QStringLiteral("XDG_RUNTIME_DIR"),
             QStringLiteral("XDG_DATA_DIRS"), QStringLiteral("XDG_CONFIG_DIRS")}) {
            const QString directory = tree.filePath(variable);
            QVERIFY(QDir().mkpath(directory));
            environment.insert(variable, directory);
        }
        environment.insert("XDG_CURRENT_DESKTOP", "X-Generic");
        environment.insert("DBUS_SESSION_BUS_ADDRESS", "unix:path=" + tree.filePath("no-bus"));
        environment.remove("KDE_SESSION_VERSION");
        environment.remove("BROWSER");
        environment.insert("DISPLAY", ":987");
        const QString receipt = tree.filePath("receipt");
        environment.insert("LAUNCH_RECEIPT", receipt);
        const auto run = [&](const QString &program, const QStringList &arguments) {
            QProcess process;
            process.setProcessChannelMode(QProcess::MergedChannels);
            process.setProcessEnvironment(environment);
            process.start(program, arguments);
            if (!process.waitForFinished(asyncTimeout) || process.exitCode() != 0)
                return QByteArray("FAILED: ") + process.readAllStandardOutput();
            return process.readAllStandardOutput().trimmed();
        };
        QCOMPARE(run("/usr/bin/desktop-file-validate", {dataHome() + "/applications/omafiles.desktop"}), QByteArray());
        QCOMPARE(run("/usr/bin/xdg-mime", {"default", "omafiles.desktop", "inode/directory"}), QByteArray());
        QCOMPARE(run("/usr/bin/xdg-mime", {"query", "default", "inode/directory"}), QByteArray("omafiles.desktop"));
        const QString folder = tree.filePath(QStringLiteral("folder space \"$`%;\u03bb"));
        QVERIFY(QDir().mkpath(folder));
        const QString uri = QUrl::fromLocalFile(folder).toString(QUrl::FullyEncoded);
        const auto recorded = [&] {
            QFile output(receipt);
            return output.open(QIODevice::ReadOnly) ? output.readAll() : QByteArray();
        };
        QCOMPARE(run("/usr/bin/xdg-open", {uri}), QByteArray());
        QTRY_COMPARE(recorded(), uri.toUtf8() + '\0');
        QVERIFY(QFile::remove(receipt));
        QCOMPARE(run("/usr/bin/xdg-open", {folder}), QByteArray());
        QTRY_COMPARE(recorded(), folder.toUtf8() + '\0');
        QVERIFY(QFile::remove(receipt));
        const QString second = tree.filePath("second folder");
        QVERIFY(QDir().mkpath(second));
        QCOMPARE(run("/usr/bin/gio", {"launch", dataHome() + "/applications/omafiles.desktop",
            uri, QUrl::fromLocalFile(second).toString(QUrl::FullyEncoded)}), QByteArray());
        QTRY_COMPARE(recorded(), folder.toUtf8() + '\0' + second.toUtf8() + '\0');
        QVERIFY(!QFile::exists(tree.filePath("sentinel")));
    }
}

void TestStudio::refusesUnquotableDefaultExecutables()
{
    const QByteArray data = qgetenv("XDG_DATA_HOME"), config = qgetenv("XDG_CONFIG_HOME");
    const auto restore = qScopeGuard([&] {
        qputenv("XDG_DATA_HOME", data);
        qputenv("XDG_CONFIG_HOME", config);
    });
    QStringList executables = {QStringLiteral("studio"), QString()};
    const QString characters = QStringLiteral(" \t\n\r\"'\\><~|&;$*?#()`%=")
        + QChar(0) + QChar(1) + QChar(127);
    for (const QChar character : characters)
        executables.append(QStringLiteral("/tmp/studio") + character);
    for (bool existing : {false, true}) {
        for (const QString &executable : executables) {
            QTemporaryDir tree;
            qputenv("XDG_DATA_HOME", tree.filePath("data").toUtf8());
            qputenv("XDG_CONFIG_HOME", tree.filePath("config").toUtf8());
            const QStringList paths = {dataHome() + "/applications/omafiles.desktop",
                dataHome() + "/dbus-1/services/org.freedesktop.FileManager1.service",
                configHome() + "/mimeapps.list", dataHome() + "/applications/mimeapps.list"};
            if (existing) {
                for (const QString &path : paths) {
                    QVERIFY(QDir().mkpath(typedDirectory(path, nullptr)));
                    QFile file(path);
                    QVERIFY(file.open(QIODevice::WriteOnly));
                    QCOMPARE(file.write("original"), qint64(8));
                }
            }
            openRequests().clear();
            const auto result = makeDefaultFileManager(executable);
            QVERIFY(!result.ok);
            QVERIFY(result.text.contains("absolute"));
            QVERIFY(result.text.contains("reserved Desktop Entry"));
            QVERIFY(result.text.contains("mise run install"));
            QVERIFY(result.text.contains("from PATH"));
            QVERIFY(openRequests().isEmpty());
            for (const QString &path : paths) {
                QCOMPARE(QFile::exists(path), existing);
                if (existing) {
                    QFile file(path);
                    QVERIFY(file.open(QIODevice::ReadOnly));
                    QCOMPARE(file.readAll(), QByteArray("original"));
                }
                QVERIFY(!QFile::exists(path + ".omafiles-backup"));
                QVERIFY(!QFile::exists(path + ".new"));
            }
        }
    }
}

void TestStudio::presentsWithTheCallersActivationToken()
{
    const QByteArray token = qgetenv("XDG_ACTIVATION_TOKEN");
    const QByteArray startup = qgetenv("DESKTOP_STARTUP_ID");
    const auto restore = qScopeGuard([&] {
        qputenv("XDG_ACTIVATION_TOKEN", token);
        qputenv("DESKTOP_STARTUP_ID", startup);
    });
    qputenv("DESKTOP_STARTUP_ID", "legacy-id");
    qputenv("XDG_ACTIVATION_TOKEN", "wayland-token");
    QCOMPARE(FileManager::launchToken(), QStringLiteral("wayland-token"));
    qunsetenv("XDG_ACTIVATION_TOKEN");
    QCOMPARE(FileManager::launchToken(), QStringLiteral("legacy-id"));
    Workspace workspace;
    FileManager manager;
    manager.setWorkspace(&workspace);
    QWindow target;
    manager.setWindow(&target);
    QByteArray observed;
    connect(&target, &QWindow::visibleChanged, &target, [&](bool visible) {
        if (visible)
            observed = qgetenv("XDG_ACTIVATION_TOKEN");
    });
    manager.ShowFolders({QUrl::fromLocalFile(m_tree.path()).toString()}, "dbus-token");
    QCOMPARE(observed, QByteArray("dbus-token"));
    QVERIFY(qgetenv("XDG_ACTIVATION_TOKEN").isEmpty());
    QVERIFY(target.isVisible());
    target.hide();
    Workspace subsequent;
    QCOMPARE(subsequent.currentTab()->cwd(), homeDirectory());
}

void TestStudio::createsTheFirstTagFromTheEmptyPanel()
{
    const auto restore = qScopeGuard([&] { writeIndex(); m_index->reload(); settle(); });
    QFile index(defaultIndexPath());
    QVERIFY(index.open(QIODevice::WriteOnly | QIODevice::Truncate));
    index.close();
    m_index->reload();
    settle();
    settle();
    QCOMPARE(m_index->tagCount(), 0);
    focusRow("plain.txt");
    verbRuns().clear();
    key('t');
    QCOMPARE(window()->property("focusRegion").toString(), QStringLiteral("tags"));
    key('a');
    auto *sheet = itemsNamed("tagSheet").first();
    QVERIFY(sheet->isVisible());
    QVERIFY(window()->property("blocked").toBool());
    for (char ch : QByteArray("first-tag"))
        key(ch);
    key(Qt::Key_Down);
    key(Qt::Key_Return);
    QCOMPARE(verbRuns(), (QVector<QStringList>{{"tag", "add", "--tag", "first-tag", "--",
        m_tree.filePath("plain.txt")}}));
    QVERIFY(!sheet->isVisible());
    QVERIFY(m_index->notice().contains("first-tag"));
    key('j');
    QVERIFY(!window()->property("blocked").toBool());
}

void TestStudio::completesAndRenamesTagsOnTheMarkedSet()
{
    focusRow("one.txt");
    key(Qt::Key_Space);
    focusRow("two.md");
    key(Qt::Key_Space);
    focusRow("one.txt");
    const QStringList targets = qobject_cast<FolderModel *>(filesRoot()->property("folder").value<QObject *>())
        ->targetPaths(m_tree.filePath("one.txt"));
    QCOMPARE(targets.size(), 2);
    verbRuns().clear();
    key('t');
    key('r');
    auto *source = itemsNamed("tagSource").first();
    auto *destination = itemsNamed("tagDestination").first();
    auto *input = source->findChild<QQuickItem *>("tagInput");
    QVERIFY(input);
    input->setProperty("text", "no");
    key(Qt::Key_Tab);
    QCOMPARE(source->property("text").toString(), QStringLiteral("notes"));
    key(Qt::Key_Return);
    QVERIFY(destination->findChild<QQuickItem *>("tagInput")->hasActiveFocus());
    for (char ch : QByteArray("research"))
        key(ch);
    key(Qt::Key_Return);
    QCOMPARE(verbRuns(), (QVector<QStringList>{QStringList{"tag", "rename", "--", "notes", "research"} + targets}));
    QVERIFY(!itemsNamed("tagSheet").first()->isVisible());
    auto *tags = window()->property("activeTagsList").value<QQuickItem *>();
    QTRY_VERIFY(tags->hasActiveFocus());
    qobject_cast<FolderModel *>(filesRoot()->property("folder").value<QObject *>())->clearMarks();
    focusRow("plain.txt");
    key('t');
    key('a');
    for (char ch : QByteArray("pro"))
        key(ch);
    key(Qt::Key_Down);
    key(Qt::Key_Return);
    QCOMPARE(source->property("text").toString(), QStringLiteral("project"));
    QCOMPARE(verbRuns().size(), 1);
    key(Qt::Key_Return);
    QCOMPARE(verbRuns().last(), (QStringList{"tag", "add", "--tag", "project", "--", m_tree.filePath("plain.txt")}));
    m_index->tagRename({}, "notes", "work");
    QCOMPARE(verbRuns().size(), 2);
}

void TestStudio::keepsTagRefusalsAndPendingTargetsInTheEditor()
{
    VerbReceipt reply;
    const auto previous = setVerbRunner([&](const QStringList &arguments, VerbReceipt done) {
        verbRuns().append(arguments);
        reply = std::move(done);
    });
    const auto restore = qScopeGuard([&] {
        setVerbRunner(previous);
        writeIndex();
        m_index->reload();
    settle();
    });
    focusRow("plain.txt");
    verbRuns().clear();
    key('t');
    key('a');
    for (char ch : QByteArray("bad-tag"))
        key(ch);
    key(Qt::Key_Return);
    QVERIFY(m_index->tagEditing());
    auto *sheet = itemsNamed("tagSheet").first();
    QVERIFY(sheet->property("waiting").toBool());
    QVERIFY(!itemsNamed("tagSource").first()->isEnabled());
    QVERIFY(!itemsNamed("tagApply").first()->isEnabled());
    auto *close = itemsNamed("tagClose").first();
    QVERIFY(close->hasActiveFocus());
    key(Qt::Key_Tab);
    QVERIFY(close->hasActiveFocus());
    key(Qt::Key_Backtab, Qt::ShiftModifier);
    QVERIFY(close->hasActiveFocus());
    QMetaObject::invokeMethod(sheet, "apply");
    m_index->tagAdd({m_tree.filePath("one.txt")}, "other");
    QCOMPARE(verbRuns().size(), 1);
    QVERIFY(reply);
    QFile partial(defaultIndexPath());
    QVERIFY(partial.open(QIODevice::WriteOnly | QIODevice::Append));
    partial.write(QStringLiteral("{\"path\":\"%1\",\"tags\":[\"partial\"]}\n")
                      .arg(m_tree.filePath("plain.txt")).toUtf8());
    partial.close();
    reply(false, QString(), "tag add: cannot lock index");
    QTRY_COMPARE(m_index->tagChoices({m_tree.filePath("plain.txt")}), QStringList{"partial"});
    settle();
    QVERIFY(!m_index->tagEditing());
    QVERIFY(sheet->isVisible());
    QVERIFY(sheet->property("refusal").toString().contains("cannot lock index"));
    QCOMPARE(itemsNamed("tagSource").first()->property("text").toString(), QStringLiteral("bad-tag"));
    QVERIFY(itemsNamed("tagSource").first()->findChild<QQuickItem *>("tagInput")->hasActiveFocus());
    key(Qt::Key_Return);
    QCOMPARE(verbRuns().size(), 2);
    key(Qt::Key_Escape);
    QVERIFY(!sheet->isVisible());
    key(Qt::Key_L, Qt::ControlModifier);
    QQuickItem *focus = window()->activeFocusItem();
    QVERIFY(focus);
    reply(true, QString(), QString());
    settle();
    QCOMPARE(window()->activeFocusItem(), focus);
    key(Qt::Key_Escape);
    QCOMPARE(verbRuns().last().last(), m_tree.filePath("plain.txt"));
}

void TestStudio::keepsTagKeyboardFocusInsideTheEditor()
{
    focusRow("plain.txt");
    verbRuns().clear();
    key('t');
    key('a');
    auto *sheet = itemsNamed("tagSheet").first();
    auto *source = itemsNamed("tagSource").first()->findChild<QQuickItem *>("tagInput");
    auto *destination = itemsNamed("tagDestination").first()->findChild<QQuickItem *>("tagInput");
    source->setProperty("text", "fresh-tag");
    const QList<QQuickItem *> chain = {itemsNamed("tagApply").first(), itemsNamed("tagClose").first(),
        itemsNamed("tagMode-add").first(), itemsNamed("tagMode-remove").first(),
        itemsNamed("tagMode-rename").first(), source};
    for (int round = 0; round < 2; ++round) {
        for (auto *control : chain) {
            key(Qt::Key_Tab);
            QVERIFY2(control->hasActiveFocus(), qPrintable(control->objectName()));
            QVERIFY(sheet->isVisible());
        }
    }
    QVERIFY(verbRuns().isEmpty());
    key(Qt::Key_Backtab, Qt::ShiftModifier);
    QVERIFY(itemsNamed("tagMode-rename").first()->hasActiveFocus());
    key(Qt::Key_Space);
    QCOMPARE(sheet->property("operation").toString(), QStringLiteral("rename"));
    QVERIFY(source->hasActiveFocus());
    key(Qt::Key_Return);
    QVERIFY(destination->hasActiveFocus());
    destination->setProperty("text", "replacement-tag");
    key(Qt::Key_Tab);
    QVERIFY(itemsNamed("tagApply").first()->hasActiveFocus());
    key(Qt::Key_Backtab, Qt::ShiftModifier);
    QVERIFY(destination->hasActiveFocus());
    key(Qt::Key_Backtab, Qt::ShiftModifier);
    QVERIFY(source->hasActiveFocus());
    source->setProperty("text", "");
    destination->setProperty("text", "");
    key(Qt::Key_Tab);
    QVERIFY(destination->hasActiveFocus());
    key(Qt::Key_Tab);
    QVERIFY(itemsNamed("tagClose").first()->hasActiveFocus());
    QVERIFY(verbRuns().isEmpty());
    key(Qt::Key_Space);
    QVERIFY(!sheet->isVisible());
    QCOMPARE(window()->property("focusRegion").toString(), QStringLiteral("tags"));
}

void TestStudio::editsTagsFromThePaletteMenuAndPointer()
{
    focusRow("plain.txt");
    verbRuns().clear();
    key(Qt::Key_P, Qt::ControlModifier);
    for (char ch : QByteArray("Add tag to"))
        key(ch);
    key(Qt::Key_Return);
    auto *sheet = itemsNamed("tagSheet").first();
    QVERIFY(sheet->isVisible());
    key(Qt::Key_Escape);
    QVERIFY(verbRuns().isEmpty());
    key('m');
    QVERIFY(chooseInMenu("fileMenu", "Add tag…"));
    QVERIFY(sheet->isVisible());
    key(Qt::Key_Escape);
    focusRow("one.txt");
    for (auto *chip : itemsNamed("tagChip")) {
        if (!chip->isVisible() || chip->opacity() == 0 || chip->property("modelData").toString() != "work")
            continue;
        const QPoint center = chip->mapToScene(QPointF(chip->width() / 2, chip->height() / 2)).toPoint();
        QTest::mouseClick(window(), Qt::LeftButton, Qt::NoModifier, center);
        settle();
        QVERIFY(sheet->isVisible());
        QCOMPARE(sheet->property("operation").toString(), QStringLiteral("remove"));
        QCOMPARE(itemsNamed("tagSource").first()->property("text").toString(), QStringLiteral("work"));
        key(Qt::Key_Return);
        QCOMPARE(verbRuns().size(), 1);
        QCOMPARE(verbRuns().last().mid(0, 5), (QStringList{"tag", "rm", "--tag", "work", "--"}));
        return;
    }
    QFAIL("no visible tag chip to edit");
}

static bool tapItem(QQuickWindow *window, QQuickItem *item, bool hold = false, bool usePen = false)
{
    static auto *touch = QTest::createTouchDevice();
    static QPointingDevice pen("test pen", 42, QInputDevice::DeviceType::Stylus,
        QPointingDevice::PointerType::Pen,
        QInputDevice::Capability::Position | QInputDevice::Capability::Pressure, 1, 1);
    const QPointer<QQuickItem> target(item);
    if (!QQuickTest::qWaitForPolish(window, asyncTimeout) || !target)
        return false;
    const QPoint at = target->mapToScene(QPointF(target->width() / 2, target->height() / 2)).toPoint();
    const int delay = hold ? QGuiApplication::styleHints()->mousePressAndHoldInterval() + 150 : 30;
    const auto tablet = [&](bool pressed) {
        QTabletEvent event(pressed ? QEvent::TabletPress : QEvent::TabletRelease, &pen,
            at, window->mapToGlobal(at), pressed ? 1 : 0, 0, 0, 0, 0, 0,
            Qt::NoModifier, Qt::LeftButton, pressed ? Qt::LeftButton : Qt::NoButton);
        event.setTimestamp(QDateTime::currentMSecsSinceEpoch());
        QCoreApplication::sendEvent(window, &event);
    };
    if (usePen) {
        tablet(true); QTest::qWait(delay); tablet(false);
    } else {
        auto sequence = QTest::touchEvent(window, touch, false);
        sequence.press(0, at, window).commit();
        QTest::qWait(delay);
        sequence.release(0, at, window).commit();
    }
    return true;
}

void TestStudio::editsTagsWithTouchAndPen()
{
    for (bool usePen : {false, true}) {
        focusRow("one.txt");
        verbRuns().clear();
        const auto tap = [&](QQuickItem *item) {
            QVERIFY(tapItem(window(), item, false, usePen));
            settle();
        };
        QQuickItem *target = nullptr;
        for (auto *chip : itemsNamed("tagChip")) {
            if (chip->isVisible() && chip->opacity() > 0 && chip->property("modelData").toString() == "work") {
                target = chip;
                break;
            }
        }
        QVERIFY(target);
        tap(target);
        auto *sheet = itemsNamed("tagSheet").first();
        QTRY_VERIFY2(sheet->isVisible(), usePen ? "pen opens tag editor" : "touch opens tag editor");
        auto *input = itemsNamed("tagSource").first()->findChild<QQuickItem *>("tagInput");
        QVERIFY2(input->hasActiveFocus(), usePen ? "pen focus" : "touch focus");
        QVERIFY(verbRuns().isEmpty());
        tap(itemsNamed("tagMode-add").first());
        QVERIFY2(sheet->property("operation").toString() == QStringLiteral("add"), usePen ? "pen mode tap" : "touch mode tap");
        QVERIFY2(input->hasActiveFocus(), usePen ? "pen focus" : "touch focus");
        input->setProperty("text", "pointer-tag");
        tap(itemsNamed("tagApply").first());
        QCOMPARE(verbRuns(), (QVector<QStringList>{{"tag", "add", "--tag", "pointer-tag", "--",
            m_tree.filePath("one.txt")}}));
        QVERIFY(!sheet->isVisible());
    }
}


void TestStudio::exportsNativeFileDragsWithoutDeletingSources()
{
    QList<QUrl> urls;
    QByteArray encoded;
    QObject *source = nullptr;
    const auto previous = setFileDragRunner([&](QDrag *drag) {
        urls = drag->mimeData()->urls();
        encoded = drag->mimeData()->data("text/uri-list");
        source = drag->source();
        return Qt::MoveAction;
    });
    const auto restore = qScopeGuard([&] { setFileDragRunner(previous); });
    const QString one = m_tree.filePath("one.txt");
    const QString two = m_tree.filePath("two.md");
    focusRow("one.txt");
    key(Qt::Key_Space);
    focusRow("two.md");
    key(Qt::Key_Space);
    focusRow("one.txt");
    auto *folder = qobject_cast<FolderModel *>(filesRoot()->property("folder").value<QObject *>());
    const auto targets = folder->targetPaths(one);
    for (const QString &mode : {QStringLiteral("list"), QStringLiteral("details"), QStringLiteral("icons")}) {
        tab()->setViewMode(mode);
        settle();
        QQuickItem *grip = nullptr;
        for (auto *item : itemsNamed("grip")) {
            if (item->isVisible() && item->property("path").toString() == one) {
                grip = item;
                break;
            }
        }
        QVERIFY(grip);
        const QPointF start = grip->mapToScene(QPointF(grip->width() / 2, grip->height() / 2));
        static auto *touch = QTest::createTouchDevice();
        QPointingDevice pen(QStringLiteral("drag pen"), 43, QInputDevice::DeviceType::Stylus,
            QPointingDevice::PointerType::Pen,
            QInputDevice::Capability::Position | QInputDevice::Capability::Pressure, 1, 1);
        for (int input = 0; input < 3; ++input) {
            urls.clear();
            QTest::qWait(20);
            if (input == 0) {
                pressAndDrag(window(), start, start + QPointF(60, 15));
                settle();
                sendLeft(window(), QEvent::MouseButtonRelease, start + QPointF(60, 15));
            } else if (input == 1) {
                auto sequence = QTest::touchEvent(window(), touch, false);
                sequence.press(0, start.toPoint(), window()).commit();
                for (int distance : {10, 30, 60}) {
                    sequence.move(0, (start + QPointF(distance, 15)).toPoint(), window()).commit();
                    QTest::qWait(20);
                }
                sequence.release(0, (start + QPointF(60, 15)).toPoint(), window()).commit();
            } else {
                const quint64 timestamp = QDateTime::currentMSecsSinceEpoch();
                const auto sendPen = [&](QEvent::Type type, QPointF point, int step) {
                    const bool released = type == QEvent::TabletRelease;
                    QTabletEvent event(type, &pen, point, window()->mapToGlobal(point),
                        released ? 0 : 1, 0, 0, 0, 0, 0, Qt::NoModifier,
                        type == QEvent::TabletMove ? Qt::NoButton : Qt::LeftButton,
                        released ? Qt::NoButton : Qt::LeftButton);
                    event.setTimestamp(timestamp + step);
                    QCoreApplication::sendEvent(window(), &event);
                };
                sendPen(QEvent::TabletPress, start, 0);
                for (int distance : {10, 30, 60}) {
                    sendPen(QEvent::TabletMove, start + QPointF(distance, 15), distance);
                    QTest::qWait(20);
                }
                sendPen(QEvent::TabletRelease, start + QPointF(60, 15), 70);
            }
            QTRY_COMPARE(urls.size(), 2);
            QStringList exported;
            for (const auto &url : urls)
                exported.append(url.toLocalFile());
            QCOMPARE(exported, targets);
            QCOMPARE(source, window());
            QVERIFY(QFileInfo::exists(one));
            QVERIFY(QFileInfo::exists(two));
            QVERIFY(!filesRoot()->property("band").value<QObject *>()->property("live").toBool());
        }
    }
    FileTransfer transfer;
    const QString special = m_tree.filePath("space # percent %.txt");
    transfer.startDrag(qobject_cast<QQuickItem *>(filesRoot()), {special});
    QCOMPARE(urls, QList<QUrl>{QUrl::fromLocalFile(special)});
    QVERIFY(encoded.contains("space%20%23%20percent%20%25.txt"));
    folder->clearMarks();
    tab()->setViewMode("list");
}

static QList<QUrl> invalidFileTransferUrls()
{
    return {QUrl("https://example.org/file"), QUrl("file://remote/tmp/file"),
            QUrl("file:///tmp/name%FF"), QUrl("file:///tmp/name?query"),
            QUrl("file:///tmp/name%2Fchild")};
}

void TestStudio::receivesLocalFileDropsWithConventionalActions()
{
    FileTransfer transfer;
    QSignalSpy requested(&transfer, &FileTransfer::requested);
    QSignalSpy refused(&transfer, &FileTransfer::refused);
    const QString one = m_tree.filePath("one.txt");
    const QString dir = m_tree.filePath("alpha");
    for (bool same : {false, true}) {
        QCOMPARE(fileDropAction(same, Qt::ControlModifier), Qt::CopyAction);
        QCOMPARE(fileDropAction(same, Qt::ShiftModifier), Qt::MoveAction);
        QCOMPARE(fileDropAction(same, Qt::ControlModifier | Qt::ShiftModifier), Qt::LinkAction);
        QCOMPARE(fileDropAction(same, Qt::NoModifier), same ? Qt::MoveAction : Qt::CopyAction);
    }
    for (const auto modifiers : std::initializer_list<Qt::KeyboardModifiers>{Qt::NoModifier, Qt::ControlModifier, Qt::ShiftModifier,
                                Qt::ControlModifier | Qt::ShiftModifier}) {
        requested.clear();
        QVERIFY(transfer.receive({QUrl::fromLocalFile(one), QUrl::fromLocalFile(one)}, dir,
                                  modifiers, Qt::CopyAction | Qt::MoveAction));
        QTRY_COMPARE(requested.size(), 1);
        QCOMPARE(requested.first().at(0).toStringList(), QStringList{one});
        QCOMPARE(requested.first().at(1).toString(), dir);
        QCOMPARE(requested.first().at(2).toInt(), int(fileDropAction(true, modifiers)));
    }
    requested.clear();
    QVERIFY(transfer.receive({QUrl::fromLocalFile("/proc/self/status")}, dir, 0, Qt::CopyAction));
    QTRY_COMPARE(requested.size(), 1);
    QCOMPARE(requested.first().at(2).toInt(), int(Qt::CopyAction));
    requested.clear();
    QVERIFY(transfer.receive({QUrl::fromLocalFile(one), QUrl::fromLocalFile("/proc/self/status")},
                             dir, 0, Qt::CopyAction));
    QTRY_COMPARE(requested.size(), 1);
    QCOMPARE(requested.first().at(2).toInt(), int(Qt::CopyAction));
    requested.clear();
    for (const QUrl &url : invalidFileTransferUrls()) {
        QVERIFY(!transfer.receive({QUrl::fromLocalFile(one), url}, dir, 0, Qt::CopyAction));
    }
    QVERIFY(!transfer.receive({QUrl::fromLocalFile(one)}, dir, 0, Qt::MoveAction));
    QCOMPARE(refused.size(), 6);
    QVERIFY(requested.isEmpty());
    QVERIFY(transfer.receive({QUrl::fromLocalFile(m_tree.filePath("missing"))}, dir, 0, Qt::CopyAction));
    QTRY_COMPARE(refused.size(), 7);
    QVERIFY(requested.isEmpty());
    QVERIFY(transfer.receive({QUrl::fromLocalFile(one)}, one, 0, Qt::CopyAction));
    QTRY_COMPARE(refused.size(), 8);
    QVERIFY(requested.isEmpty());
}

bool TestStudio::cancelPendingFileDrop()
{
    auto *transfer = m_engine->singletonInstance<FileTransfer *>("omafiles", "FileTransfer");
    auto *conflict = itemsNamed("conflictSheet").first();
    return QTest::qWaitFor([&] {
        if (conflict->isVisible()) QMetaObject::invokeMethod(conflict, "close");
        return transfer->findChildren<QFutureWatcherBase *>().isEmpty()
            && !m_index->operationBusy() && !conflict->isVisible();
    }, asyncTimeout);
}

static bool sendFileDrop(QQuickItem *item, const QStringList &paths,
                         Qt::KeyboardModifiers modifiers = Qt::NoModifier,
                         const QPointF &relative = QPointF(0.5, 0.5))
{
    const QPointer<QQuickItem> target(item);
    const QPointer<QQuickWindow> window(item ? item->window() : nullptr);
    if (!window || !QQuickTest::qWaitForPolish(window, asyncTimeout)
        || !target || target->window() != window)
        return false;
    const QPointF point = target->mapToScene(QPointF(target->width() * relative.x(),
                                                   target->height() * relative.y()));
    std::unique_ptr<QMimeData> mime(fileMimeData(paths, false));
    const Qt::DropActions actions = Qt::CopyAction | Qt::MoveAction | Qt::LinkAction;
    QDragEnterEvent enter(point.toPoint(), actions, mime.get(), Qt::LeftButton, modifiers);
    QCoreApplication::sendEvent(window, &enter);
    if (!enter.isAccepted())
        return false;
    QDropEvent drop(point, actions, mime.get(), Qt::LeftButton, modifiers);
    QCoreApplication::sendEvent(window, &drop);
    return drop.isAccepted() && drop.dropAction() == Qt::CopyAction;
}

void TestStudio::mapsDropTargetsAfterTheirRowReflows_data()
{
    QTest::addColumn<bool>("accepted");
    QTest::newRow("accepts-target") << true;
    QTest::newRow("rejects-target") << false;
}

void TestStudio::mapsDropTargetsAfterTheirRowReflows()
{
    QFETCH(bool, accepted);
    QQuickWindow scene;
    scene.resize(400, 120);
    QQmlComponent component(m_engine);
    component.setData("import QtQuick; Row {}", QUrl());
    QScopedPointer<QQuickItem> row(qobject_cast<QQuickItem *>(component.create()));
    QVERIFY2(row, qPrintable(component.errorString()));
    row->setParentItem(scene.contentItem());
    DropReceiver neighbor(row.data());
    DropReceiver target(row.data());
    neighbor.setSize(QSizeF(100, 80));
    target.setSize(QSizeF(100, 80));
    neighbor.accepts = !accepted;
    target.accepts = accepted;
    scene.show();
    QVERIFY(QTest::qWaitForWindowExposed(&scene, asyncTimeout));
    QVERIFY(QQuickTest::qWaitForPolish(&scene, asyncTimeout));
    QCOMPARE(target.x(), 100);

    neighbor.setWidth(200);
    QVERIFY(QQuickTest::qIsPolishScheduled(row.data()));
    QCOMPARE(target.x(), 100);
    QCOMPARE(sendFileDrop(&target, {m_tree.filePath("one.txt")}), accepted);
    QCOMPARE(target.x(), 200);
    QCOMPARE(neighbor.enters, 0);
    QCOMPARE(neighbor.drops, 0);
    QCOMPARE(target.enters, 1);
    QCOMPARE(target.drops, accepted ? 1 : 0);
    if (accepted) QCOMPARE(target.position, QPointF(50, 40));
}

void TestStudio::avoidsDropsOnTargetsDestroyedDuringPolish()
{
    QQuickWindow scene;
    scene.resize(300, 120);
    DropReceiver neighbor(scene.contentItem());
    neighbor.setSize(QSizeF(300, 120));
    neighbor.accepts = true;
    QPointer<DropReceiver> target = new DropReceiver(scene.contentItem());
    target->setSize(QSizeF(100, 80));
    ScenePolish layout(scene.contentItem());
    layout.action = [&] { delete target.data(); };
    scene.show();
    QVERIFY(QTest::qWaitForWindowExposed(&scene, asyncTimeout));
    layout.polish();
    QVERIFY(QQuickTest::qIsPolishScheduled(&layout));
    QVERIFY(!sendFileDrop(target, {m_tree.filePath("one.txt")}));
    QVERIFY(layout.completed);
    QVERIFY(target.isNull());
    QCOMPARE(neighbor.enters, 0);
    QCOMPARE(neighbor.drops, 0);
}

void TestStudio::refusesVirtualTabDropsWhileTheStripReflows()
{
    QTemporaryDir destination;
    QVERIFY(destination.isValid());
    const QString occupied = destination.filePath("one.txt");
    QVERIFY(writeLooknfeel(occupied, "occupied"));
    auto *folderTab = tab();
    folderTab->setCwd(destination.path());
    settle();
    m_workspace->addTab("tags");
    settle();
    QVERIFY(QQuickTest::qWaitForPolish(window(), asyncTimeout));
    QPointer<QQuickItem> folderHandle;
    QPointer<QQuickItem> virtualArea;
    for (auto *handle : itemsNamed("tabHandle")) {
        if (handle->property("current").toBool())
            virtualArea = handle->findChild<QQuickItem *>("fileDropArea");
        else
            folderHandle = handle;
    }
    QVERIFY(folderHandle);
    QVERIFY(virtualArea);
    const qreal originalWidth = folderHandle->width();
    const auto restore = qScopeGuard([&] {
        const bool finished = cancelPendingFileDrop();
        if (folderHandle) folderHandle->setWidth(originalWidth);
        m_workspace->closeTab(0, m_workspace->paneAtIndex(0)->currentIndex());
        folderTab->setCwd(m_tree.path());
        QVERIFY2(finished, "The unexpected drop must finish cancellation before its destination retires");
    });
    QVERIFY(!virtualArea->isEnabled());
    QVERIFY(virtualArea->property("directory").toString().isEmpty());
    auto *folderArea = folderHandle->findChild<QQuickItem *>("fileDropArea");
    QVERIFY(folderArea);
    QVERIFY(folderArea->isEnabled());
    QCOMPARE(folderArea->property("directory").toString(), destination.path());
    const QPointF stale = virtualArea->mapToScene(virtualArea->boundingRect().center());
    folderHandle->setWidth(originalWidth + virtualArea->width());
    QVERIFY(QQuickTest::qIsPolishScheduled(folderHandle->parentItem()));
    QVERIFY(QRectF(folderArea->mapToScene(QPointF()), folderArea->size()).contains(stale));
    auto *transfer = m_engine->singletonInstance<FileTransfer *>("omafiles", "FileTransfer");
    QVERIFY(transfer);
    QSignalSpy requested(transfer, &FileTransfer::requested);
    const bool accepted = sendFileDrop(virtualArea, {m_tree.filePath("one.txt")});
    if (accepted) {
        QTRY_VERIFY(itemsNamed("conflictSheet").first()->isVisible());
        qInfo() << "Stale tab coordinate opened the adjacent folder's conflict sheet";
    }
    QVERIFY2(!accepted, "A virtual tab drop reached the adjacent folder during reflow");
    QVERIFY(requested.isEmpty());
    QVERIFY(!window()->property("blocked").toBool());
    QFile file(occupied);
    QVERIFY(file.open(QIODevice::ReadOnly));
    QCOMPARE(file.readAll(), QByteArray("occupied"));
}

void TestStudio::drainsDropConflictsThatArriveDuringCleanup()
{
    QTemporaryDir destination;
    QVERIFY(destination.isValid());
    const QString occupied = destination.filePath("one.txt");
    QVERIFY(writeLooknfeel(occupied, "occupied"));
    const QString sourcePath = m_tree.filePath("one.txt");
    QFile source(sourcePath);
    QVERIFY(source.open(QIODevice::ReadOnly));
    const QByteArray original = source.readAll();
    source.close();
    auto *origin = tab();
    origin->setCwd(destination.path());
    const auto restore = qScopeGuard([&] {
        const bool finished = cancelPendingFileDrop();
        origin->setCwd(m_tree.path());
        QVERIFY2(finished, "A pending drop must finish before its temporary destination retires");
    });
    settle();
    QQuickItem *target = nullptr;
    for (auto *area : itemsNamed("fileDropArea"))
        if (area->isVisible() && area->height() > 200
                && area->property("directory").toString() == destination.path()) target = area;
    QVERIFY(target);
    QVERIFY(QQuickTest::qWaitForPolish(window(), asyncTimeout));
    auto *pool = QThreadPool::globalInstance();
    pool->waitForDone();
    const int threads = pool->maxThreadCount();
    pool->setMaxThreadCount(1);
    QSemaphore entered, release;
    QTimer resume;
    resume.setSingleShot(true);
    connect(&resume, &QTimer::timeout, &resume, [&] { release.release(); });
    const auto unblock = qScopeGuard([&] {
        resume.stop();
        release.release();
        pool->waitForDone();
        pool->setMaxThreadCount(threads);
    });
    const auto blocker = QtConcurrent::run([&] { entered.release(); release.acquire(); });
    QVERIFY(entered.tryAcquire(1, asyncTimeout));
    QSignalSpy requested(m_index, &IndexModel::conflictRequested);
    QVERIFY(sendFileDrop(target, {sourcePath}, Qt::ControlModifier, QPointF(0.95, 0.95)));
    auto *transfer = m_engine->singletonInstance<FileTransfer *>("omafiles", "FileTransfer");
    QCOMPARE(transfer->findChildren<QFutureWatcherBase *>().size(), 1);
    QVERIFY(!m_index->operationBusy());
    QVERIFY(!itemsNamed("conflictSheet").first()->isVisible());
    QVERIFY(requested.isEmpty());
    resume.start(0);
    QVERIFY(cancelPendingFileDrop());
    QCOMPARE(requested.size(), 1);
    QVERIFY(!m_index->operationBusy());
    QVERIFY(!window()->property("blocked").toBool());
    QVERIFY(source.open(QIODevice::ReadOnly));
    QCOMPARE(source.readAll(), original);
    QFile file(occupied);
    QVERIFY(file.open(QIODevice::ReadOnly));
    QCOMPARE(file.readAll(), QByteArray("occupied"));
}

void TestStudio::dropsIntoFolderRowsTabsAndOtherPanes()
{
    const QString one = m_tree.filePath("one.txt");
    const QString alpha = m_tree.filePath("alpha");
    const auto dropOn = [&](QQuickItem *target, const QString &directory,
                            Qt::KeyboardModifiers modifiers) {
        verbRuns().clear();
        QVERIFY(sendFileDrop(target, {one}, modifiers));
        QTRY_COMPARE(verbRuns().size(), 1);
        QCOMPARE(verbRuns().first(), (QStringList{modifiers == Qt::ControlModifier ? "cp" : "mv", "--", one, directory}));
    };
    focusRow("alpha");
    QQuickItem *rowTarget = nullptr;
    for (auto *area : itemsNamed("fileDropArea"))
        if (area->isVisible() && area->property("directory").toString() == alpha)
            rowTarget = area;
    QVERIFY(rowTarget);
    dropOn(rowTarget, alpha, Qt::NoModifier);
    key('t', Qt::ControlModifier);
    tab()->setCwd(alpha);
    settle();
    const auto handles = itemsNamed("tabHandle");
    QVERIFY(handles.size() >= 2);
    QQuickItem *handle = nullptr;
    for (auto *item : handles)
        if (item->property("current").toBool())
            handle = item;
    QVERIFY(handle);
    key(Qt::Key_Backtab, Qt::ControlModifier | Qt::ShiftModifier);
    dropOn(handle, alpha, Qt::ControlModifier);
    QCOMPARE(cwd(), m_tree.path());
    m_workspace->splitRight(QStringLiteral("files"));
    settle();
    tab()->setCwd(alpha);
    settle();
    QQuickItem *floor = nullptr;
    for (auto *area : itemsNamed("fileDropArea"))
        if (area->isVisible() && area->property("directory").toString() == alpha
            && area->height() > 100)
            floor = area;
    QVERIFY(floor);
    dropOn(floor, alpha, Qt::ShiftModifier);
    verbRuns().clear();
    QMetaObject::invokeMethod(window(), "takeFocus");
    key('?');
    QVERIFY(window()->property("blocked").toBool());
    QVERIFY(!sendFileDrop(floor, {one}, Qt::NoModifier, QPointF(0.95, 0.95)));
    QVERIFY(verbRuns().isEmpty());
    key(Qt::Key_Escape);
    key(Qt::Key_L, Qt::ControlModifier);
    QVERIFY(window()->property("editingPath").toBool());
    QVERIFY(!sendFileDrop(floor, {one}, Qt::NoModifier, QPointF(0.95, 0.95)));
    QVERIFY(verbRuns().isEmpty());
    key(Qt::Key_Escape);
    const QString link = QDir(alpha).filePath("one.txt");
    const auto removeLink = qScopeGuard([&] { QFile::remove(link); });
    QVERIFY(sendFileDrop(floor, {one}, Qt::ControlModifier | Qt::ShiftModifier,
                         QPointF(0.95, 0.95)));
    QTRY_VERIFY(QFileInfo(link).isSymLink());
    QCOMPARE(QFileInfo(link).symLinkTarget(), one);
    QVERIFY(QFileInfo::exists(one));
    QVERIFY(verbRuns().isEmpty());
}

void TestStudio::linksClipboardFilesByKeyboard()
{
    const QString one = m_tree.filePath("one.txt");
    const QString two = m_tree.filePath("two.md");
    const QString plain = m_tree.filePath("plain.txt");
    const QString alpha = m_tree.filePath("alpha");
    const QString link = QDir(alpha).filePath("one.txt");
    const QString second = QDir(alpha).filePath("two.md");
    const QString third = QDir(alpha).filePath("plain.txt");
    auto *folder = qobject_cast<FolderModel *>(filesRoot()->property("folder").value<QObject *>());
    folder->clearMarks();
    const auto restore = qScopeGuard([&] {
        QFile::remove(link); QFile::remove(second); QFile::remove(third);
        folder->clearMarks(); m_clipboard->clear();
    });
    focusRow("one.txt"); key(Qt::Key_Space);
    focusRow("two.md"); key(Qt::Key_Space);
    focusRow("one.txt");
    key(Qt::Key_C, Qt::ControlModifier);
    QCOMPARE(m_clipboard->files().size(), 2);
    tab()->setCwd(alpha);
    settle();
    key(Qt::Key_V, Qt::ControlModifier | Qt::ShiftModifier);
    QTRY_VERIFY(QFileInfo(link).isSymLink());
    QTRY_VERIFY(QFileInfo(second).isSymLink());
    QCOMPARE(QFileInfo(link).symLinkTarget(), one);
    QCOMPARE(QFileInfo(second).symLinkTarget(), two);
    QVERIFY(m_clipboard->holdsFiles());
    key(Qt::Key_V, Qt::ControlModifier | Qt::ShiftModifier);
    QTRY_VERIFY(m_index->notice().contains("already exists"));
    QVERIFY(QFile::remove(link));
    const QString absent = m_tree.filePath("absent");
    QVERIFY(QFile::link(absent, link));
    m_clipboard->copyFiles({one, two, plain}, true);
    key(Qt::Key_V, Qt::ControlModifier | Qt::ShiftModifier);
    QTRY_VERIFY(m_index->notice().startsWith("linked 1 of 3;"));
    QVERIFY(m_index->notice().contains("already exists"));
    QCOMPARE(QFileInfo(link).symLinkTarget(), absent);
    QCOMPARE(QFileInfo(second).symLinkTarget(), two);
    QCOMPARE(QFileInfo(third).symLinkTarget(), plain);
    QVERIFY(m_clipboard->cutting());
    QVERIFY(QFileInfo::exists(one));
    QVERIFY(QFileInfo::exists(two));
    QVERIFY(QFileInfo::exists(plain));
}

void TestStudio::refusesDropsIntoVirtualViews()
{
    const QString one = m_tree.filePath("one.txt");
    m_clipboard->copyFiles({one}, false);
    const auto clear = qScopeGuard([&] { m_clipboard->clear(); });
    for (int mode = 0; mode < 5; ++mode) {
        if (mode == 0)
            tab()->setSelectedTag("work");
        else if (mode == 1)
            tab()->setPattern("*.txt");
        else if (mode == 2)
            tab()->setRecent(true);
        else
            m_workspace->addTab(mode == 3 ? "tags" : "graph");
        settle();
        QVERIFY(!window()->property("canPaste").toBool());
        verbRuns().clear();
        for (auto *handle : itemsNamed("tabHandle")) {
            if (!handle->property("current").toBool())
                continue;
            auto *area = handle->findChild<QQuickItem *>("fileDropArea");
            QVERIFY(area);
            QVERIFY(!area->isEnabled());
            QVERIFY(area->property("directory").toString().isEmpty());
            QVERIFY(!sendFileDrop(area, {one}));
        }
        for (auto *area : itemsNamed("fileDropArea")) {
            if (!area->isVisible() || area->height() < 200)
                continue;
            QVERIFY(area->property("directory").toString().isEmpty());
            QVERIFY(!sendFileDrop(area, {one}, Qt::NoModifier, QPointF(0.95, 0.95)));
        }
        key(Qt::Key_V, Qt::ControlModifier);
        key(Qt::Key_V, Qt::ControlModifier | Qt::ShiftModifier);
        QVERIFY(verbRuns().isEmpty());
        QVERIFY(QFileInfo::exists(one));
        if (mode < 3) {
            tab()->setSelectedTag({});
            tab()->setPattern({});
            tab()->setRecent(false);
        } else {
            m_workspace->closeTab(0, m_workspace->paneAtIndex(0)->currentIndex());
        }
        settle();
    }
}

void TestStudio::rejectsMixedClipboardFileUrls()
{
    const QString one = m_tree.filePath("one.txt");
    const auto clear = qScopeGuard([&] { m_clipboard->clear(); });
    for (const QUrl &url : invalidFileTransferUrls()) {
        auto *mime = fileMimeData({one}, true);
        const QList<QUrl> urls{QUrl::fromLocalFile(one), url};
        mime->setUrls(urls);
        QGuiApplication::clipboard()->setMimeData(mime);
        QVERIFY(m_clipboard->files().isEmpty());
        QVERIFY(!m_clipboard->holdsFiles());
        verbRuns().clear();
        key(Qt::Key_V, Qt::ControlModifier);
        key(Qt::Key_V, Qt::ControlModifier | Qt::ShiftModifier);
        QVERIFY(verbRuns().isEmpty());
        QCOMPARE(QGuiApplication::clipboard()->mimeData()->urls(), urls);
        QVERIFY(QFileInfo::exists(one));
    }
    auto *mime = fileMimeData({one, one}, false);
    QGuiApplication::clipboard()->setMimeData(mime);
    QCOMPARE(m_clipboard->files(), QStringList{one});
}

static void dragOverlayContent(QQuickWindow *window, QQuickItem *view)
{
    static auto *touch = QTest::createTouchDevice();
    auto sequence = QTest::touchEvent(window, touch, false);
    const QPoint start = view->mapToScene(QPointF(view->width() / 2, view->height() - 20)).toPoint();
    sequence.press(0, start, window).commit();
    for (int distance : {20, 50, 90, 140}) {
        sequence.move(0, start - QPoint(0, distance), window).commit();
        QTest::qWait(20);
    }
    sequence.release(0, start - QPoint(0, 140), window).commit();
}

void TestStudio::scrollsLongOverlayListsWithPointerAndKeyboard()
{
    const QSize original = window()->size();
    const auto restore = qScopeGuard([&] { window()->resize(original); });
    filesRoot()->setProperty("enabled", false);
    const auto enableFiles = qScopeGuard([&] { filesRoot()->setProperty("enabled", true); });
    QVariantList actions;
    for (int i = 0; i < 80; ++i)
        actions.append(QVariantMap{{"id", QString::number(i)},
            {"label", QString(240, 'x') + QString::number(i)}, {"key", "Ctrl Shift Enter"}});
    for (const QString &type : {QStringLiteral("ContextMenu"), QStringLiteral("CommandPalette")}) {
        QQmlComponent component(m_engine, QUrl("qrc:/qml/" + type + ".qml"));
        QScopedPointer<QQuickItem> overlay(qobject_cast<QQuickItem *>(component.create()));
        QVERIFY2(overlay, qPrintable(component.errorString()));
        overlay->setParentItem(window()->contentItem());
        overlay->setZ(200);
        overlay->setSize(window()->size());
        overlay->setProperty(type == "ContextMenu" ? "actions" : "commands", actions);
        QSignalSpy chosen(overlay.data(), SIGNAL(chosen(QString)));
        if (type == "ContextMenu")
            QVERIFY(QMetaObject::invokeMethod(overlay.data(), "openAt", Q_ARG(QVariant, 1179), Q_ARG(QVariant, 719)));
        else
            QVERIFY(QMetaObject::invokeMethod(overlay.data(), "open"));
        settle();
        auto *view = overlay->findChild<QQuickItem *>("overlayList");
        QVERIFY(view);
        auto *panel = view->parentItem();
        if (type != "ContextMenu")
            panel = panel->parentItem();
        const auto contained = [&] {
            const QRectF rect(panel->mapToScene(QPointF()), panel->size());
            return QRectF(QPointF(), window()->size()).contains(rect);
        };
        QVERIFY(contained());
        key(Qt::Key_End, type == "ContextMenu" ? Qt::NoModifier : Qt::ControlModifier);
        QTRY_COMPARE(view->property("currentIndex").toInt(), 79);
        auto *current = view->property("currentItem").value<QQuickItem *>();
        QVERIFY(current);
        QVERIFY(current->mapToItem(view, QPointF()).y() >= -0.5);
        QVERIFY(current->mapToItem(view, QPointF(0, current->height())).y() <= view->height() + 0.5);
        key(Qt::Key_Tab);
        QCOMPARE(view->property("currentIndex").toInt(), 0);
        key(Qt::Key_Tab, Qt::ShiftModifier);
        QCOMPARE(view->property("currentIndex").toInt(), 79);
        auto *focus = window()->activeFocusItem();
        QVERIFY(focus);
        while (focus && focus != overlay.data())
            focus = focus->parentItem();
        QVERIFY(focus == overlay.data());
        key(Qt::Key_Home, type == "ContextMenu" ? Qt::NoModifier : Qt::ControlModifier);
        QTRY_COMPARE(view->property("currentIndex").toInt(), 0);
        settle();
        const QPointF point = view->mapToScene(QPointF(view->width() / 2, view->height() / 2));
        QWheelEvent wheel(point, window()->mapToGlobal(point.toPoint()), {}, QPoint(0, -120),
            Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
        QCoreApplication::sendEvent(window(), &wheel);
        QVERIFY(view->property("contentY").toDouble() > 0);
        view->setProperty("contentY", 0);
        dragOverlayContent(window(), view);
        QTRY_VERIFY(view->property("contentY").toDouble() > 0);
        QCOMPARE(chosen.size(), 0);
        window()->resize(880, 480);
        overlay->setSize(window()->size());
        settle();
        QVERIFY(contained());
        QMetaObject::invokeMethod(overlay.data(), "close");
        window()->resize(original);
    }
    filesRoot()->setProperty("enabled", true);
    QMetaObject::invokeMethod(window(), "takeFocus");
    focusRow("one.txt");
    key(Qt::Key_Menu);
    auto *menu = itemsNamed("fileMenu").first();
    QVERIFY(menu->isVisible());
    auto *view = menu->findChild<QQuickItem *>("overlayList");
    QVERIFY(view);
    QTRY_VERIFY(view->property("contentHeight").toDouble() > view->height());
    dragOverlayContent(window(), view);
    QTRY_VERIFY(view->property("contentY").toDouble() > 0);
    QVERIFY(menu->isVisible());
    key(Qt::Key_End);
    auto *current = view->property("currentItem").value<QQuickItem *>();
    QVERIFY(current);
    QVERIFY(current->mapToItem(view, QPointF(0, current->height())).y() <= view->height() + 0.5);
    key(Qt::Key_Escape);
    for (const QString &name : {QStringLiteral("fileMenu"), QStringLiteral("tabMenu"),
         QStringLiteral("openWithMenu"), QStringLiteral("scriptsMenu"), QStringLiteral("filterMenu"),
         QStringLiteral("viewMenu"), QStringLiteral("columnMenu"), QStringLiteral("newTabMenu")}) {
        auto *popup = itemsNamed(name).first();
        QVERIFY(QMetaObject::invokeMethod(popup, "openAt", Q_ARG(QVariant, 500), Q_ARG(QVariant, 300)));
        QVERIFY2(window()->property("overlayOpen").toBool(), qPrintable(name));
        QVERIFY(!filesRoot()->property("enabled").toBool());
        QMetaObject::invokeMethod(popup, "close");
    }
    QMetaObject::invokeMethod(window(), "takeFocus");
}

void TestStudio::keepsTagEditorFocusAndSuggestionsVisible()
{
    const QSize original = window()->size();
    const auto restore = qScopeGuard([&] { window()->resize(original); });
    window()->resize(880, 480);
    focusRow("one.txt");
    key('t'); key('a');
    auto *sheet = itemsNamed("tagSheet").first();
    QVERIFY(sheet->isVisible());
    sheet->setProperty("operation", "rename");
    sheet->setProperty("refusal", QString("A detailed refusal. ").repeated(120));
    sheet->setProperty("existing", QStringList{"a", "b", "c", "d", "e", "f"});
    auto *scroll = sheet->findChild<QQuickItem *>("overlayScroll");
    QVERIFY(scroll);
    auto *source = itemsNamed("tagSource").first();
    QMetaObject::invokeMethod(source, "focusEntry");
    settle();
    for (int i = 0; i < 6; ++i)
        key(Qt::Key_Down);
    auto *suggestions = source->findChild<QQuickItem *>("tagSuggestions");
    auto *current = suggestions->property("currentItem").value<QQuickItem *>();
    QVERIFY(current);
    QTRY_VERIFY(current->mapToItem(scroll, QPointF()).y() >= -0.5);
    QTRY_VERIFY(current->mapToItem(scroll, QPointF(0, current->height())).y() <= scroll->height() + 0.5);
    window()->resize(320, 180);
    settle();
    QCOMPARE(suggestions->property("currentIndex").toInt(), 5);
    QTRY_VERIFY(current->mapToItem(scroll, QPointF()).y() >= -0.5);
    QTRY_VERIFY(current->mapToItem(scroll, QPointF(0, current->height())).y() <= scroll->height() + 0.5);
    auto *input = window()->activeFocusItem();
    QVERIFY(input);
    QTRY_VERIFY(input->mapToItem(scroll, QPointF()).y() >= -0.5);
    QTRY_VERIFY(input->mapToItem(scroll, QPointF(0, input->height())).y() <= scroll->height() + 0.5);
    key(Qt::Key_Up);
    QCOMPARE(suggestions->property("currentIndex").toInt(), 4);
    QTRY_VERIFY(input->mapToItem(scroll, QPointF()).y() >= -0.5);
    key(Qt::Key_Tab, Qt::ShiftModifier);
    for (int i = 0; i < 9; ++i) {
        key(Qt::Key_Tab);
        settle();
        auto *focus = window()->activeFocusItem();
        QVERIFY(focus);
        const QPointF top = focus->mapToItem(scroll, QPointF());
        QVERIFY2(top.y() >= -0.5, qPrintable(QString("%1 %2 top=%3 height=%4 scroll=%5 contentY=%6")
            .arg(i).arg(focus->metaObject()->className()).arg(top.y()).arg(focus->height())
            .arg(scroll->height()).arg(scroll->property("contentY").toReal())));
        QVERIFY2(top.y() + focus->height() <= scroll->height() + 0.5, qPrintable(focus->objectName()));
    }
    key(Qt::Key_Escape);
}

void TestStudio::routesSidebarKeysByScrollRange_data()
{
    QTest::addColumn<bool>("pendingRefresh");
    QTest::addColumn<int>("width");
    QTest::addColumn<bool>("overflowing");
    QTest::addColumn<bool>("tooltip");
    QTest::addColumn<int>("navigationKey");
    for (bool pendingRefresh : {false, true})
        for (int width : {1180, 420})
            for (bool overflowing : {false, true})
                for (bool tooltip : {false, true})
                    for (auto code : {Qt::Key_Up, Qt::Key_Down, Qt::Key_PageUp,
                             Qt::Key_PageDown, Qt::Key_Home, Qt::Key_End})
                        QTest::newRow(qPrintable(QString("%1-%2-%3-%4").arg(width)
                            .arg(overflowing ? "overflow" : "fits").arg(tooltip ? "tip" : "no-tip").arg(code)
                            + (pendingRefresh ? "-pending-refresh" : "")))
                            << pendingRefresh << width << overflowing << tooltip << int(code);
}

void TestStudio::routesSidebarKeysByScrollRange()
{
    QFETCH(bool, pendingRefresh);
    QFETCH(int, width);
    QFETCH(bool, overflowing);
    QFETCH(bool, tooltip);
    QFETCH(int, navigationKey);
    const QSize original = window()->size();
    const auto restore = qScopeGuard([&] { window()->resize(original); });
    window()->resize(width, overflowing ? 320 : width < 640 ? 800 : 720);
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    for (int i = 0; i < 120; ++i) {
        QFile file(directory.filePath(QString("file-%1.txt").arg(i, 3, 10, QLatin1Char('0'))));
        QVERIFY(file.open(QIODevice::WriteOnly));
    }
    tab()->setCwd(directory.path());
    auto *folder = filesRoot()->property("folder").value<QObject *>();
    QTRY_COMPARE_WITH_TIMEOUT(folder->property("count").toInt(), 121, asyncTimeout);
    QVERIFY(QMetaObject::invokeMethod(filesRoot(), "setCursor", Q_ARG(QVariant, QVariant(50))));
    if (width < 640) key('t');
    auto *dock = window()->property("activeDock").value<QQuickItem *>();
    QVERIFY(dock);
    auto *scroll = dock->findChild<QQuickItem *>("dockScroll");
    auto *bits = dock->findChild<QQuickItem *>("detailsMode");
    QVERIFY(scroll && bits);
    auto *control = bits->property("firstControl").value<QQuickItem *>();
    QVERIFY(control);
    QTRY_VERIFY_WITH_TIMEOUT(control->isVisible() && control->isEnabled(), asyncTimeout);
    QSignalSpy publication(folder, SIGNAL(loadingChanged()));
    QVERIFY(publication.isValid());
    if (pendingRefresh) {
        QTimer::singleShot(0, folder, [folder] {
            QVERIFY(QMetaObject::invokeMethod(folder, "reload"));
        });
    }
    QTest::mouseMove(window(), QPoint(2, 2));
    // A pending publication can disable this control and legitimately move focus.
    settle();
    QVERIFY(QQuickTest::qWaitForPolish(window(), asyncTimeout));
    QTRY_VERIFY_WITH_TIMEOUT(control->isVisible() && control->isEnabled(), asyncTimeout);
    if (pendingRefresh) QVERIFY(publication.size() >= 2);
    QCOMPARE(cursor(), 50);
    control->forceActiveFocus();
    QCOMPARE(scroll->property("contentHeight").toReal() > scroll->height(), overflowing);
    QTRY_VERIFY(control->hasActiveFocus());
    QTRY_VERIFY(hasFocusOutline(window(), control, m_theme->accent()));
    QObject *tip = nullptr;
    if (tooltip) {
        control->setProperty("tooltipCaption", QStringLiteral("Read permission"));
        for (auto *child : control->children())
            if (QByteArray(child->metaObject()->className()).startsWith("QQuickLoader"))
                child->setProperty("active", true);
        tip = control->property("tooltip").value<QObject *>();
        QVERIFY(tip);
        tip->setProperty("text", QStringLiteral("Read permission"));
        QVERIFY(QMetaObject::invokeMethod(tip, "request", Q_ARG(QVariant, QVariant::fromValue(control))));
        QTRY_VERIFY_WITH_TIMEOUT(tip->property("opened").toBool(), asyncTimeout);
        auto *tipScroll = tip->findChild<QQuickItem *>("overlayScroll");
        QVERIFY(tipScroll && tipScroll->property("contentHeight").toReal() <= tipScroll->height());
    }
    const auto cancel = qScopeGuard([&] {
        if (tip) QMetaObject::invokeMethod(tip, "cancel");
        QTest::mouseMove(window(), QPoint(2, 2));
    });
    key(Qt::Key_Right);
    QCOMPARE(window()->activeFocusItem(), bits->property("controls").toList().at(1).value<QQuickItem *>());
    key(Qt::Key_Left);
    QVERIFY(control->hasActiveFocus());
    const qreal maximum = qMax(0.0, scroll->property("contentHeight").toReal() - scroll->height());
    scroll->setProperty("contentY", maximum / 2);
    const qreal before = scroll->property("contentY").toReal();
    const QString path = filesRoot()->property("entry").toMap().value("path").toString();
    const auto permissions = QFileInfo(path).permissions();
    key(Qt::Key(navigationKey));
    const bool forwards = navigationKey == Qt::Key_Down || navigationKey == Qt::Key_PageDown
        || navigationKey == Qt::Key_End;
    if (overflowing) {
        QCOMPARE(cursor(), 50);
        const qreal after = scroll->property("contentY").toReal();
        QVERIFY(forwards ? after > before : after < before);
        QVERIFY(scroll->hasActiveFocus());
    } else {
        QVERIFY(forwards ? cursor() > 50 : cursor() < 50);
        if (navigationKey == Qt::Key_Home) QCOMPARE(cursor(), 0);
        if (navigationKey == Qt::Key_End) QCOMPARE(cursor(), 120);
        QCOMPARE(scroll->property("contentY").toReal(), 0.0);
        if (navigationKey != Qt::Key_Home) QVERIFY(control->hasActiveFocus());
    }
    QCOMPARE(QFileInfo(path).permissions(), permissions);
    if (overflowing || navigationKey != Qt::Key_Home)
        QTRY_VERIFY(hasFocusOutline(window(), overflowing ? scroll : control, m_theme->accent()));
}

void TestStudio::declinesNavigationWithoutScrollRange_data()
{
    QTest::addColumn<QString>("type");
    for (const QString type : {"Dock", "HelpSheet", "ModeSheet", "TagSheet", "ArchiveSheet",
             "ConflictSheet", "PreviewSheet", "Tooltip", "FileChooser", "Properties"})
        QTest::newRow(qPrintable(type)) << type;
}

void TestStudio::declinesNavigationWithoutScrollRange()
{
    QFETCH(QString, type);
    QScopedPointer<QObject> owned;
    QObject *container = nullptr;
    if (type == "Properties") {
        QVERIFY(QMetaObject::invokeMethod(window(), "showReceipt", Q_ARG(QVariant, QStringLiteral("Test")),
            Q_ARG(QVariant, QStringLiteral("Read-only properties"))));
        QTRY_VERIFY(window()->findChild<QObject *>("propertiesDialog"));
        container = window()->findChild<QObject *>("propertiesDialog");
    } else {
        QQmlComponent component(m_engine, QUrl("qrc:/qml/" + type + ".qml"));
        owned.reset(component.create());
        QVERIFY2(owned, qPrintable(component.errorString()));
        container = owned.data();
        if (auto *item = qobject_cast<QQuickItem *>(container)) {
            item->setParentItem(window()->contentItem());
            item->setSize(QSizeF(880, 480));
        }
        if (auto *loader = container->findChild<QObject *>("sheetContentLoader"))
            loader->setProperty("active", true);
        if (type == "ConflictSheet") container->setProperty("active", true);
        if (type == "PreviewSheet") container->setProperty("initialized", true);
    }
    const auto close = qScopeGuard([&] {
        if (type == "Properties") QMetaObject::invokeMethod(container, "close");
    });
    const auto items = container->findChildren<QQuickItem *>();
    int checked = 0;
    for (auto *scroll : items) {
        if (scroll->objectName() != "overlayScroll" && scroll->objectName() != "dockScroll"
                && scroll->objectName() != "chooserForm") continue;
        ++checked;
        scroll->setProperty("focusOnScroll", true);
        QQmlExpression geometry(qmlContext(scroll), scroll, "anchors.fill = undefined; anchors.top = undefined; anchors.bottom = undefined; height = 100");
        geometry.evaluate();
        QVERIFY2(!geometry.hasError(), qPrintable(geometry.error().toString()));
        for (qreal content : {80.0, 100.0, 500.0, 80.0}) {
            scroll->setProperty("contentHeight", content);
            for (auto code : {Qt::Key_Up, Qt::Key_Down, Qt::Key_PageUp,
                     Qt::Key_PageDown, Qt::Key_Home, Qt::Key_End}) {
                const qreal maximum = qMax(0.0, content - scroll->height());
                for (int modifiers : {int(Qt::ShiftModifier), int(Qt::ControlModifier),
                         int(Qt::ControlModifier | Qt::ShiftModifier), int(Qt::AltModifier),
                         int(Qt::MetaModifier), int(Qt::KeypadModifier),
                         int(Qt::ControlModifier | Qt::AltModifier | Qt::ShiftModifier)}) {
                    scroll->setProperty("contentY", maximum / 2);
                    auto *focused = window()->activeFocusItem();
                    QQmlExpression modified(qmlContext(scroll), scroll,
                        QString("(function() { const event = {key: %1, modifiers: %2, accepted: false}; "
                            "const handled = handleKey(event); return [handled, event.accepted]; })()")
                            .arg(code).arg(modifiers));
                    const auto answer = modified.evaluate().toList();
                    QVERIFY2(!modified.hasError(), qPrintable(modified.error().toString()));
                    QCOMPARE(answer, (QVariantList{false, false}));
                    QCOMPARE(scroll->property("contentY").toReal(), maximum / 2);
                    QCOMPARE(window()->activeFocusItem(), focused);
                }
                scroll->setProperty("contentY", maximum / 2);
                QQmlExpression expression(qmlContext(scroll), scroll,
                    QString("(function() { const event = {key: %1, modifiers: 0, accepted: false}; "
                        "const handled = handleKey(event); return [handled, event.accepted]; })()").arg(code));
                const QVariantList result = expression.evaluate().toList();
                QVERIFY2(!expression.hasError(), qPrintable(expression.error().toString()));
                QCOMPARE(result, (QVariantList{maximum > 0, maximum > 0}));
                const qreal position = scroll->property("contentY").toReal();
                const bool forwards = code == Qt::Key_Down || code == Qt::Key_PageDown || code == Qt::Key_End;
                if (maximum > 0) QVERIFY2(forwards ? position > maximum / 2 : position < maximum / 2,
                    qPrintable(QString("%1 key=%2 height=%3 content=%4 max=%5 y=%6")
                        .arg(type).arg(code).arg(scroll->height()).arg(scroll->property("contentHeight").toReal()).arg(maximum).arg(position)));
                else QCOMPARE(position, 0.0);
            }
        }
    }
    QVERIFY2(checked > 0, qPrintable(type));
}


void TestStudio::preservesTextSelectionInResultEditors_data()
{
    QTest::addColumn<bool>("palette");
    QTest::addColumn<int>("width");
    QTest::addColumn<bool>("selected");
    for (bool palette : {false, true})
        for (int width : {1180, 420})
            for (bool selected : {false, true})
                QTest::newRow(qPrintable(QString("%1-%2-%3").arg(palette ? "palette" : "tags")
                    .arg(width).arg(selected ? "selection" : "caret"))) << palette << width << selected;
}

void TestStudio::preservesTextSelectionInResultEditors()
{
    QFETCH(bool, palette);
    QFETCH(int, width);
    QFETCH(bool, selected);
    const QSize original = window()->size();
    const auto restore = qScopeGuard([&] {
        for (const QString &name : {QStringLiteral("tagSheet"), QStringLiteral("commandPalette")})
            QMetaObject::invokeMethod(itemsNamed(name).first(), "close");
        window()->resize(original);
        writeIndex();
        m_index->reload();
        QVERIFY(QTest::qWaitFor([&] { return !m_index->loading(); }, asyncTimeout));
    });
    if (!palette) {
        Store store(defaultIndexPath());
        QString why;
        QVERIFY2(store.lockAndLoad(&why), qPrintable(why));
        Record record = store.get(m_tree.filePath("one.txt"));
        record.tags.clear();
        for (int i = 0; i < 32; ++i)
            record.tags.append(QString("tag-%1").arg(i, 2, 10, QLatin1Char('0')));
        store.put(record);
        QVERIFY2(store.commit(&why), qPrintable(why));
        m_index->reload();
        settle();
    }
    window()->resize(width, palette ? 300 : 220);
    focusRow("one.txt");
    key(':');
    if (!palette) {
        for (char c : QByteArray("Add tag to the row")) key(c);
        key(Qt::Key_Return);
    }
    auto *overlay = itemsNamed(palette ? "commandPalette" : "tagSheet").first();
    QTRY_VERIFY(overlay->isVisible());
    auto *input = palette ? overlay->findChild<QQuickItem *>("paletteQuery")
        : itemsNamed("tagSource").first()->findChild<QQuickItem *>("tagInput");
    auto *results = overlay->findChild<QQuickItem *>(palette ? "overlayList" : "tagSuggestions");
    auto *scroll = palette ? results : overlay->findChild<QQuickItem *>("overlayScroll");
    QVERIFY(input && results && scroll);
    const QString text = palette ? QStringLiteral("e") : QStringLiteral("tag-");
    for (QChar c : text) key(c.toLatin1());
    QTRY_VERIFY(results->property("count").toInt() >= (palette ? 16 : 6));
    for (int i = 0; i < (palette ? 12 : 4); ++i) key(Qt::Key_Down);
    QCOMPARE(results->property("currentIndex").toInt(), palette ? 12 : 3);
    key(Qt::Key_Up);
    QCOMPARE(results->property("currentIndex").toInt(), palette ? 11 : 2);
    key(Qt::Key_Down);
    QCOMPARE(results->property("currentIndex").toInt(), palette ? 12 : 3);
    QTRY_VERIFY(scroll->property("contentHeight").toReal() > scroll->height());
    if (palette) {
        key(Qt::Key_PageUp);
        QVERIFY(results->property("currentIndex").toInt() < 12);
        key(Qt::Key_PageDown);
        QCOMPARE(results->property("currentIndex").toInt(), 12);
        key(Qt::Key_Home, Qt::ControlModifier);
        QCOMPARE(results->property("currentIndex").toInt(), 0);
        key(Qt::Key_End, Qt::ControlModifier);
        QCOMPARE(results->property("currentIndex").toInt(), results->property("count").toInt() - 1);
        key('p', Qt::ControlModifier);
        const int previous = results->property("currentIndex").toInt();
        key('n', Qt::ControlModifier);
        QCOMPARE(results->property("currentIndex").toInt(), previous + 1);
        key(Qt::Key_Home, Qt::ControlModifier);
        for (int i = 0; i < 12; ++i) key(Qt::Key_Down);
    }

    QQmlComponent component(m_engine);
    component.setData("import QtQuick; TextInput { width: 400; height: 28; x: -1000 }", QUrl());
    QScopedPointer<QQuickItem> reference(qobject_cast<QQuickItem *>(component.create()));
    QVERIFY2(reference, qPrintable(component.errorString()));
    reference->setParentItem(overlay);
    reference->setProperty("text", text);
    const auto prepare = [&](QQuickItem *editor) {
        editor->forceActiveFocus();
        editor->setProperty("cursorPosition", text.size());
        if (selected)
            QMetaObject::invokeMethod(editor, "select", Q_ARG(int, 0), Q_ARG(int, text.size()));
        settle();
    };
    const auto runs = verbRuns();
    for (int modifiers : {int(Qt::ShiftModifier), int(Qt::ControlModifier | Qt::ShiftModifier),
             int(Qt::AltModifier), int(Qt::MetaModifier), int(Qt::ControlModifier)}) {
        for (auto code : {Qt::Key_Up, Qt::Key_Down, Qt::Key_PageUp, Qt::Key_PageDown,
                 Qt::Key_Home, Qt::Key_End, Qt::Key_Left, Qt::Key_Right}) {
            if (palette && modifiers == Qt::ControlModifier
                    && (code == Qt::Key_Home || code == Qt::Key_End)) continue;
            if (palette) {
                input->forceActiveFocus();
                key(Qt::Key_Home, Qt::ControlModifier);
                for (int i = 0; i < 12; ++i) key(Qt::Key_Down);
            }
            prepare(reference.data());
            key(code, Qt::KeyboardModifiers(modifiers));
            QCOMPARE(window()->activeFocusItem(), reference.data());
            const int cursor = reference->property("cursorPosition").toInt();
            const QString selection = reference->property("selectedText").toString();
            prepare(input);
            QVERIFY(QQuickTest::qWaitForPolish(window(), asyncTimeout));
            const QPointF point = scroll->mapToScene(QPointF(4, 4));
            QWheelEvent wheel(point, window()->mapToGlobal(point.toPoint()), {}, QPoint(0, -120),
                Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
            QCoreApplication::sendEvent(window(), &wheel);
            settle();
            const qreal y = scroll->property("contentY").toReal();
            QVERIFY2(y > 0, "The selection gesture must run with an already scrolled viewport");
            const int row = results->property("currentIndex").toInt();
            QVERIFY(row > 0);
            const qreal innerY = results->property("contentY").toReal();
            key(code, Qt::KeyboardModifiers(modifiers));
            QCOMPARE(input->property("cursorPosition").toInt(), cursor);
            QCOMPARE(input->property("selectedText").toString(), selection);
            QCOMPARE(window()->activeFocusItem(), input);
            QCOMPARE(results->property("currentIndex").toInt(), row);
            QCOMPARE(results->property("contentY").toReal(), innerY);
            QCOMPARE(scroll->property("contentY").toReal(), y);
            QCOMPARE(input->property("text").toString(), text);
            QCOMPARE(verbRuns(), runs);
        }
    }
}

void TestStudio::preservesNativeTextKeysInScrollAncestors_data()
{
    QTest::addColumn<QString>("type");
    QTest::addColumn<bool>("overflowing");
    for (const QString type : {"TextInput", "ReadOnlyText"})
        for (bool overflowing : {false, true})
            QTest::newRow(qPrintable(type + (overflowing ? "-overflow" : "-fitting")))
                << type << overflowing;
}

void TestStudio::preservesNativeTextKeysInScrollAncestors()
{
    QFETCH(QString, type);
    QFETCH(bool, overflowing);
    const bool enabled = filesRoot()->property("enabled").toBool();
    filesRoot()->setProperty("enabled", false);
    const auto restore = qScopeGuard([&] {
        filesRoot()->setProperty("enabled", enabled);
        QMetaObject::invokeMethod(window(), "takeFocus");
    });
    QQmlComponent component(m_engine);
    const QByteArray definition = QByteArray("import QtQuick\nItem { width: 500; height: 400; ")
        + type.toUtf8() + " { objectName: 'reference'; width: 300; text: 'alpha beta gamma\\nsecond delta omega\\nthird line'; } "
        "OverlayScroll { objectName: 'subject'; y: 100; width: 400; height: 100; contentHeight: "
        + (overflowing ? "500" : "100") + "; focusOnScroll: true; "
        + type.toUtf8() + " { objectName: 'editor'; width: 300; text: 'alpha beta gamma\\nsecond delta omega\\nthird line'; } } }";
    component.setData(definition, QUrl("qrc:/qml/NativeKeyFixture.qml"));
    QScopedPointer<QQuickItem> fixture(qobject_cast<QQuickItem *>(component.create()));
    QVERIFY2(fixture, qPrintable(component.errorString()));
    fixture->setParentItem(window()->contentItem());
    fixture->setZ(200);
    auto *reference = fixture->findChild<QQuickItem *>("reference");
    auto *editor = fixture->findChild<QQuickItem *>("editor");
    auto *scroll = fixture->findChild<QQuickItem *>("subject");
    QVERIFY(reference && editor && scroll);
    QVERIFY(QQuickTest::qWaitForPolish(window(), asyncTimeout));
    for (int modifiers : {int(Qt::ShiftModifier), int(Qt::ControlModifier),
             int(Qt::ControlModifier | Qt::ShiftModifier), int(Qt::AltModifier),
             int(Qt::MetaModifier), int(Qt::ControlModifier | Qt::AltModifier | Qt::ShiftModifier)}) {
        for (auto code : {Qt::Key_Up, Qt::Key_Down, Qt::Key_PageUp,
                 Qt::Key_PageDown, Qt::Key_Home, Qt::Key_End}) {
            reference->forceActiveFocus();
            reference->setProperty("cursorPosition", 8);
            key(code, Qt::KeyboardModifiers(modifiers));
            const int position = reference->property("cursorPosition").toInt();
            const QString selection = reference->property("selectedText").toString();
            editor->forceActiveFocus();
            editor->setProperty("cursorPosition", 8);
            QVERIFY(QQuickTest::qWaitForPolish(window(), asyncTimeout));
            scroll->setProperty("contentY", overflowing ? 100.0 : 0.0);
            key(code, Qt::KeyboardModifiers(modifiers));
            QCOMPARE(editor->property("cursorPosition").toInt(), position);
            QCOMPARE(editor->property("selectedText").toString(), selection);
            QCOMPARE(window()->activeFocusItem(), editor);
            QCOMPARE(scroll->property("contentY").toReal(), overflowing ? 100.0 : 0.0);
            QCOMPARE(editor->property("text"), reference->property("text"));
        }
    }
}

void TestStudio::preservesModifiedKeysInPreviewAndProperties_data()
{
    QTest::addColumn<bool>("properties");
    QTest::addColumn<bool>("overflowing");
    QTest::addColumn<int>("width");
    for (bool properties : {false, true})
        for (bool overflowing : {false, true})
            for (int width : {1180, 420})
                QTest::newRow(qPrintable(QString("%1-%2-%3").arg(properties ? "properties" : "preview")
                    .arg(overflowing ? "overflow" : "fitting").arg(width))) << properties << overflowing << width;
}

void TestStudio::preservesModifiedKeysInPreviewAndProperties()
{
    QFETCH(bool, properties);
    QFETCH(bool, overflowing);
    QFETCH(int, width);
    const QSize original = window()->size();
    window()->resize(width, 720);
    QTemporaryDir fixture(m_tree.filePath("modified-preview-XXXXXX"));
    QVERIFY(fixture.isValid());
    QString content;
    for (int i = 0; i < (overflowing ? 500 : 2); ++i)
        content += QString("Line %1: alpha beta gamma delta epsilon\n").arg(i, 3, 10, QLatin1Char('0'));
    QFile source(fixture.filePath("text.txt"));
    QVERIFY(source.open(QIODevice::WriteOnly));
    QCOMPARE(source.write(content.toUtf8()), content.toUtf8().size());
    source.close();
    QObject *overlay = nullptr;
    const auto restore = qScopeGuard([&] {
        if (overlay) QMetaObject::invokeMethod(overlay, "close");
        window()->resize(original);
        tab()->setCwd(m_tree.path());
    });
    tab()->setCwd(fixture.path());
    focusRow("text.txt");
    const int fileCursor = cursor();
    QQuickItem *text = nullptr;
    if (properties) {
        QVERIFY(QMetaObject::invokeMethod(window(), "showReceipt", Q_ARG(QVariant, QStringLiteral("Test")),
            Q_ARG(QVariant, content)));
        QTRY_VERIFY(window()->findChild<QObject *>("propertiesDialog"));
        overlay = window()->findChild<QObject *>("propertiesDialog");
        text = overlay->findChild<QQuickItem *>("propertyText");
    } else {
        key('i');
        overlay = itemsNamed("previewSheet").first();
        auto *preview = qobject_cast<Preview *>(itemsNamed("quickPreview").first());
        QVERIFY(preview);
        QTRY_COMPARE_WITH_TIMEOUT(preview->text(), content, asyncTimeout);
        text = itemsNamed("previewText").first();
        for (int i = 0; i < 8 && window()->activeFocusItem() != text; ++i) key(Qt::Key_Tab);
    }
    QVERIFY(text);
    auto *scroll = overlay->findChild<QQuickItem *>("overlayScroll");
    QVERIFY(scroll);
    QTRY_COMPARE(window()->activeFocusItem(), text);
    auto *frame = text->property("focusFrame").value<QQuickItem *>();
    QVERIFY(frame);
    QTRY_VERIFY(hasFocusOutline(window(), frame, m_theme->accent()));
    QVERIFY(QQuickTest::qWaitForPolish(window(), asyncTimeout));
    QCOMPARE(scroll->property("contentHeight").toReal() > scroll->height(), overflowing);
    for (int modifiers : {int(Qt::ControlModifier | Qt::ShiftModifier), int(Qt::ShiftModifier)}) {
        for (auto code : {Qt::Key_Down, Qt::Key_Up, Qt::Key_PageDown, Qt::Key_PageUp}) {
            if (modifiers == Qt::ShiftModifier && (code == Qt::Key_Up || code == Qt::Key_Down)) continue;
            key(Qt::Key_Home, Qt::ControlModifier);
            scroll->setProperty("contentY", 0.0);
            key(code, Qt::KeyboardModifiers(modifiers));
            QCOMPARE(cursor(), fileCursor);
            QCOMPARE(window()->activeFocusItem(), text);
            const QString selected = text->property("selectedText").toString()
                .replace(QChar::ParagraphSeparator, QLatin1Char('\n'));
            if (properties && code == Qt::Key_PageDown) {
                QVERIFY(!selected.isEmpty());
                key('c', Qt::ControlModifier);
                QCOMPARE(QGuiApplication::clipboard()->text(), selected);
                QVERIFY(content.startsWith(selected));
            } else {
                QCOMPARE(text->property("cursorPosition").toInt(), 0);
                QVERIFY(selected.isEmpty());
                QCOMPARE(scroll->property("contentY").toReal(), 0.0);
            }
            QCOMPARE(text->property("text").toString(), content);
        }
    }
    if (properties) {
        for (auto modifiers : {Qt::KeyboardModifiers(Qt::ShiftModifier),
                 Qt::KeyboardModifiers(Qt::ControlModifier | Qt::ShiftModifier)}) {
            key(Qt::Key_End, Qt::ControlModifier);
            key(Qt::Key_PageUp, modifiers);
            const QString selected = text->property("selectedText").toString()
                .replace(QChar::ParagraphSeparator, QLatin1Char('\n'));
            QVERIFY(!selected.isEmpty());
            QVERIFY(content.endsWith(selected));
            key('c', Qt::ControlModifier);
            QCOMPARE(QGuiApplication::clipboard()->text(), selected);
            QCOMPARE(window()->activeFocusItem(), text);
            QCOMPARE(cursor(), fileCursor);
        }
    }
    key(Qt::Key_Home, Qt::ControlModifier);
    scroll->setProperty("contentY", 0.0);
    if (!properties) {
        key(Qt::Key_PageDown);
        QCOMPARE(scroll->property("contentY").toReal() > 0, overflowing);
        key(Qt::Key_Home);
        QCOMPARE(scroll->property("contentY").toReal(), 0.0);
    }
    key('a', Qt::ControlModifier); key('c', Qt::ControlModifier);
    QCOMPARE(QGuiApplication::clipboard()->text(), content);
}

void TestStudio::retainsNativeNavigationInFittingControls_data()
{
    QTest::addColumn<bool>("deferredResize");
    QTest::newRow("direct-resize") << false;
    QTest::newRow("pending-polish-resize") << true;
}

void TestStudio::retainsNativeNavigationInFittingControls()
{
    QFETCH(bool, deferredResize);
    const QSize original = window()->size();
    const auto restore = qScopeGuard([&] { window()->resize(original); });
    window()->resize(1180, 900);
    focusRow("one.txt");
    key('t'); key('a');
    auto *sheet = itemsNamed("tagSheet").first();
    auto *scroll = sheet->findChild<QQuickItem *>("overlayScroll");
    auto *source = sheet->property("source").value<QObject *>();
    QVERIFY(scroll && source);
    source->setProperty("text", QStringLiteral("abcdef"));
    QMetaObject::invokeMethod(source, "focusEntry");
    QTRY_VERIFY(scroll->property("contentHeight").toReal() <= scroll->height());
    auto *input = window()->activeFocusItem();
    QVERIFY(input);
    key(Qt::Key_End); QCOMPARE(input->property("cursorPosition").toInt(), 6);
    key(Qt::Key_Home); QCOMPARE(input->property("cursorPosition").toInt(), 0);
    key(Qt::Key_End, Qt::ShiftModifier);
    QCOMPARE(input->property("selectedText").toString(), QStringLiteral("abcdef"));
    key(Qt::Key_Escape);
    for (const QString &type : {QStringLiteral("ContextMenu"), QStringLiteral("CommandPalette")}) {
        QQmlComponent component(m_engine, QUrl("qrc:/qml/" + type + ".qml"));
        QScopedPointer<QQuickItem> overlay(qobject_cast<QQuickItem *>(component.create()));
        QVERIFY2(overlay, qPrintable(component.errorString()));
        overlay->setParentItem(window()->contentItem());
        overlay->setSize(QSizeF(880, 480));
        const QVariantList actions{QVariantMap{{"id", "a"}, {"label", "First"}},
            QVariantMap{{"id", "b"}, {"label", "Second"}}, QVariantMap{{"id", "c"}, {"label", "Third"}}};
        overlay->setProperty(type == "ContextMenu" ? "actions" : "commands", actions);
        if (type == "ContextMenu")
            QVERIFY(QMetaObject::invokeMethod(overlay.data(), "openAt", Q_ARG(QVariant, 20), Q_ARG(QVariant, 20)));
        else QVERIFY(QMetaObject::invokeMethod(overlay.data(), "open"));
        auto *list = overlay->property("list").value<QQuickItem *>();
        QVERIFY(list);
        QQmlExpression geometry(qmlContext(list), list,
            "anchors.fill = undefined; anchors.top = undefined; anchors.bottom = undefined");
        geometry.evaluate();
        QVERIFY2(!geometry.hasError(), qPrintable(geometry.error().toString()));
        ScenePolish layout(window()->contentItem());
        InputPressObserver inputOrder;
        inputOrder.polished = &layout.completed;
        window()->installEventFilter(&inputOrder);
        const QList<qreal> heights = deferredResize ? QList<qreal>{40, 200} : QList<qreal>{200, 40};
        for (qreal height : heights) {
            layout.completed = !deferredResize;
            if (deferredResize) {
                layout.action = [list, height] { list->setHeight(height); };
                layout.polish();
                QVERIFY(QQuickTest::qIsPolishScheduled(&layout));
            } else {
                list->setHeight(height);
                QTRY_COMPARE(list->height(), height);
                QTRY_COMPARE(list->property("count").toInt(), 3);
                QTRY_COMPARE(list->property("contentHeight").toReal() > list->height(), height == 40);
            }
            const int page = std::max(1, int(height / 28) - 1);
            if (deferredResize) {
                key(Qt::Key_PageDown);
                QCOMPARE(list->property("currentIndex").toInt(), std::min(2, page));
                QCOMPARE(list->height(), height);
                QCOMPARE(list->property("count").toInt(), 3);
                QCOMPARE(list->property("contentHeight").toReal() > list->height(), height == 40);
                QVERIFY(layout.completed);
                QVERIFY(inputOrder.presses > 0);
                QVERIFY(inputOrder.allAfterPolish);
                key(Qt::Key_Home, type == "CommandPalette" ? Qt::ControlModifier : Qt::NoModifier);
            }
            key(Qt::Key_End, type == "CommandPalette" ? Qt::ControlModifier : Qt::NoModifier);
            QCOMPARE(list->property("currentIndex").toInt(), 2);
            key(Qt::Key_Home, type == "CommandPalette" ? Qt::ControlModifier : Qt::NoModifier);
            QCOMPARE(list->property("currentIndex").toInt(), 0);
            key(Qt::Key_Down); QCOMPARE(list->property("currentIndex").toInt(), 1);
            key(Qt::Key_Up); QCOMPARE(list->property("currentIndex").toInt(), 0);
            key(Qt::Key_PageDown);
            QCOMPARE(list->property("currentIndex").toInt(), std::min(2, page));
            key(Qt::Key_PageUp); QCOMPARE(list->property("currentIndex").toInt(), 0);
        }
        layout.completed = false;
        layout.action = [list] { list->setWidth(list->width() + 1); };
        layout.polish();
        QVERIFY(QQuickTest::qIsPolishScheduled(&layout));
        key(type == "CommandPalette" ? 'n' : 'j',
            type == "CommandPalette" ? Qt::ControlModifier : Qt::NoModifier);
        QCOMPARE(list->property("currentIndex").toInt(), 1);
        QVERIFY(layout.completed);
        QVERIFY(inputOrder.allAfterPolish);
        key(type == "CommandPalette" ? 'p' : 'k',
            type == "CommandPalette" ? Qt::ControlModifier : Qt::NoModifier);
        QCOMPARE(list->property("currentIndex").toInt(), 0);
        QMetaObject::invokeMethod(overlay.data(), "close");
    }
}

void TestStudio::routesTooltipKeysOnlyWhenContentOverflows_data()
{
    QTest::addColumn<QString>("state");
    QTest::addColumn<int>("navigationKey");
    for (const QString &state : {QStringLiteral("short"), QStringLiteral("long"),
             QStringLiteral("shortened"), QStringLiteral("expanded")}) {
        for (auto entry : {qMakePair(Qt::Key_Up, "Up"), qMakePair(Qt::Key_Down, "Down"),
                 qMakePair(Qt::Key_PageUp, "PageUp"), qMakePair(Qt::Key_PageDown, "PageDown"),
                 qMakePair(Qt::Key_Home, "Home"), qMakePair(Qt::Key_End, "End")}) {
            QTest::newRow(qPrintable(state + "-" + entry.second)) << state << int(entry.first);
        }
    }
}

void TestStudio::routesTooltipKeysOnlyWhenContentOverflows()
{
    QFETCH(QString, state);
    QFETCH(int, navigationKey);
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    for (int i = 0; i < 100; ++i) {
        QFile file(directory.filePath(QString("file-%1.txt").arg(i, 3, 10, QLatin1Char('0'))));
        QVERIFY(file.open(QIODevice::WriteOnly));
    }
    tab()->setCwd(directory.path());
    auto *folder = filesRoot()->property("folder").value<QObject *>();
    QVERIFY(folder);
    QTRY_COMPARE(folder->property("count").toInt(), 101);
    QQuickItem *control = nullptr;
    for (auto *item : itemsNamed(QString()))
        if (item->isVisible() && item->property("tooltipCaption").toString() == "Sort by name")
            control = item;
    QVERIFY(control);
    const QString caption = control->property("tooltipCaption").toString();
    const auto restore = qScopeGuard([&] {
        control->setProperty("tooltipCaption", caption);
        if (auto *tip = control->property("tooltip").value<QObject *>())
            QMetaObject::invokeMethod(tip, "cancel");
        QTest::mouseMove(window(), QPoint(2, 2));
    });
    const QString longText = QString("Long accessible help. ").repeated(500);
    control->setProperty("tooltipCaption", state == "long" || state == "shortened" ? longText : caption);
    const QPoint point = control->mapToScene(QPointF(control->width()/2, control->height()/2)).toPoint();
    QTest::mouseMove(window(), QPoint(2, 2));
    QTest::mouseMove(window(), point);
    QTest::mouseClick(window(), Qt::LeftButton, Qt::NoModifier, point);
    QTRY_VERIFY(control->property("tooltip").value<QObject *>());
    auto *tip = control->property("tooltip").value<QObject *>();
    QTRY_VERIFY(tip->property("opened").toBool());
    if (state == "shortened" || state == "expanded") {
        const QString text = state == "shortened" ? caption : longText;
        control->setProperty("tooltipCaption", text);
        tip->setProperty("text", text);
    }
    auto *scroll = tip->findChild<QQuickItem *>("overlayScroll");
    QVERIFY(scroll);
    const bool overflowing = state == "long" || state == "expanded";
    QTRY_COMPARE(scroll->property("contentHeight").toReal() > scroll->height(), overflowing);
    QVERIFY(control->hasActiveFocus());
    QVERIFY(QMetaObject::invokeMethod(filesRoot(), "setCursor", Q_ARG(QVariant, QVariant(50))));
    QCOMPARE(cursor(), 50);
    const double maximum = qMax(0.0, scroll->property("contentHeight").toReal() - scroll->height());
    scroll->setProperty("contentY", maximum / 2);
    const double before = scroll->property("contentY").toReal();
    key(Qt::Key(navigationKey));
    const bool forwards = navigationKey == Qt::Key_Down || navigationKey == Qt::Key_PageDown
        || navigationKey == Qt::Key_End;
    if (overflowing) {
        QCOMPARE(cursor(), 50);
        const double after = scroll->property("contentY").toReal();
        QVERIFY(forwards ? after > before : after < before);
        if (navigationKey == Qt::Key_Home) QCOMPARE(after, 0.0);
        if (navigationKey == Qt::Key_End) QCOMPARE(after, maximum);
    } else {
        QVERIFY(forwards ? cursor() > 50 : cursor() < 50);
        if (navigationKey == Qt::Key_Home) QCOMPARE(cursor(), 0);
        if (navigationKey == Qt::Key_End) QCOMPARE(cursor(), 100);
        QCOMPARE(scroll->property("contentY").toReal(), 0.0);
    }
    QVERIFY(control->hasActiveFocus());
    QVERIFY(tip->property("opened").toBool());
}

void TestStudio::scrollsBoundedSheetsAndTooltips()
{
    const QSize original = window()->size();
    const auto restore = qScopeGuard([&] { window()->resize(original); });
    window()->resize(880, 480);
    for (const QString &type : {QStringLiteral("ModeSheet"), QStringLiteral("HelpSheet")}) {
        QQmlComponent component(m_engine, QUrl("qrc:/qml/" + type + ".qml"));
        QScopedPointer<QQuickItem> sheet(qobject_cast<QQuickItem *>(component.create()));
        QVERIFY2(sheet, qPrintable(component.errorString()));
        sheet->setParentItem(window()->contentItem());
        sheet->setSize(window()->size());
        sheet->setZ(200);
        if (type == "ModeSheet") {
            QVERIFY(QMetaObject::invokeMethod(sheet.data(), "openFor", Q_ARG(QVariant, QStringList{"/fake"}),
                Q_ARG(QVariant, (QVariantMap{{"mine", true}}))));
            sheet->setProperty("refusal", QString("Long receipt. ").repeated(300));
        } else {
            QVariantList groups;
            for (int i = 0; i < 20; ++i)
                groups.append(QVariantMap{{"title", "group"}, {"keys", QVariantList{
                    QVariantMap{{"key", "Ctrl Enter"}, {"label", QString("readable help ").repeated(40)}}}}});
            sheet->setProperty("groups", groups);
            QVERIFY(QMetaObject::invokeMethod(sheet.data(), "open"));
        }
        settle();
        auto *scroll = sheet->findChild<QQuickItem *>("overlayScroll");
        QVERIFY(scroll);
        auto *panel = scroll->parentItem();
        QVERIFY(QRectF(QPointF(), window()->size()).contains(QRectF(panel->mapToScene({}), panel->size())));
        QTRY_VERIFY2(scroll->property("contentHeight").toReal() > scroll->height(), qPrintable(type));
        key(Qt::Key_End);
        QVERIFY(sheet->isVisible());
        QVERIFY(scroll->property("contentY").toReal() > 0);
        key(Qt::Key_Home);
        QCOMPARE(scroll->property("contentY").toReal(), 0);
        key(Qt::Key_PageDown);
        QVERIFY(scroll->property("contentY").toReal() > 0);
        if (type == "ModeSheet") {
            QQuickItem *control = nullptr;
            for (auto *item : sheet->findChildren<QQuickItem *>())
                if (item->activeFocusOnTab() && item->isEnabled() && item->isVisible())
                    control = item;
            QVERIFY(control);
            control->forceActiveFocus();
            settle();
            key(Qt::Key_PageUp);
            QVERIFY(scroll->hasActiveFocus());
            key(Qt::Key_Tab);
            auto *focus = window()->activeFocusItem();
            QVERIFY(focus && focus != scroll);
            QTRY_VERIFY(focus->mapToItem(scroll, QPointF()).y() >= -0.5);
        }
        QMetaObject::invokeMethod(sheet.data(), "close");
    }
    QQmlComponent component(m_engine, QUrl("qrc:/qml/Tooltip.qml"));
    QScopedPointer<QObject> tip(component.create());
    QVERIFY2(tip, qPrintable(component.errorString()));
    QVERIFY(tip->inherits("QQuickToolTip"));
    QCOMPARE(tip->property("delay").toInt(), 480);
    auto *host = filesRoot();
    tip->setProperty("parent", QVariant::fromValue(window()->contentItem()));
    tip->setProperty("host", QVariant::fromValue(host));
    tip->setProperty("text", QString("Long help with many words. ").repeated(500));
    QVERIFY(QMetaObject::invokeMethod(tip.data(), "place"));
    QTRY_VERIFY(tip->property("opened").toBool());
    const QRectF rect(tip->property("x").toReal(), tip->property("y").toReal(),
        tip->property("width").toReal(), tip->property("height").toReal());
    QVERIFY(QRectF(QPointF(), window()->size()).contains(rect));
    auto *scroll = tip->findChild<QQuickItem *>("overlayScroll");
    QVERIFY(scroll);
    QVERIFY(scroll->property("contentHeight").toReal() > scroll->height());
    const QPointF point = scroll->mapToScene(QPointF(scroll->width() / 2, scroll->height() / 2));
    QWheelEvent wheel(point, window()->mapToGlobal(point.toPoint()), {}, QPoint(0, -120),
        Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
    QCoreApplication::sendEvent(window(), &wheel);
    QVERIFY(scroll->property("contentY").toReal() > 0);
    QMetaObject::invokeMethod(tip.data(), "dismiss");
    QQmlComponent controlComponent(m_engine, QUrl("qrc:/qml/FocusableControl.qml"));
    QScopedPointer<QQuickItem> control(qobject_cast<QQuickItem *>(controlComponent.create()));
    QVERIFY2(control, qPrintable(controlComponent.errorString()));
    control->setParentItem(window()->contentItem());
    control->setPosition(QPointF(20, 20));
    control->setSize(QSizeF(40, 28));
    control->setZ(200);
    QVERIFY(!control->findChild<QObject *>("controlTooltip"));
    control->setProperty("tooltipCaption", QString("Long accessible tip. ").repeated(500));
    QVERIFY(!control->findChild<QObject *>("controlTooltip"));
    QTest::mouseMove(window(), QPoint(30, 30));
    auto *hoverTip = control->findChild<QObject *>("controlTooltip");
    QVERIFY(hoverTip);
    QTRY_VERIFY(hoverTip->property("opened").toBool());
    auto *tipScroll = hoverTip->findChild<QQuickItem *>("overlayScroll");
    QVERIFY(tipScroll);
    const QPoint inTip = tipScroll->mapToScene(QPointF(tipScroll->width() / 2, tipScroll->height() / 2)).toPoint();
    QTest::mouseMove(window(), inTip);
    QTest::qWait(250);
    QVERIFY(hoverTip->property("opened").toBool());
    QWheelEvent tipWheel(inTip, window()->mapToGlobal(inTip), {}, QPoint(0, -120),
        Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
    QCoreApplication::sendEvent(window(), &tipWheel);
    QVERIFY(tipScroll->property("contentY").toReal() > 0);
    tipScroll->setProperty("contentY", 0);
    dragOverlayContent(window(), tipScroll);
    QTRY_VERIFY(tipScroll->property("contentY").toReal() > 0);
    control->forceActiveFocus();
    const double beforePage = tipScroll->property("contentY").toReal();
    key(Qt::Key_PageDown);
    QVERIFY(tipScroll->property("contentY").toReal() > beforePage);
    QVERIFY(control->hasActiveFocus());
    control->setEnabled(false);
    QVERIFY(!hoverTip->property("visible").toBool());
    control->setEnabled(true);
    QMetaObject::invokeMethod(hoverTip, "request", Q_ARG(QVariant, QVariant::fromValue(control.data())));
    control->setEnabled(false);
    QTest::qWait(500);
    QVERIFY(!hoverTip->property("visible").toBool());
    m_workspace->propertiesRequested(QString("A property with a detailed value.\n").repeated(200));
    settle();
    QPointer<QObject> dialog = window()->findChild<QObject *>("propertiesDialog");
    QVERIFY(dialog);
    const auto dismiss = qScopeGuard([&] {
        if (dialog) QMetaObject::invokeMethod(dialog, "close");
    });
    auto *text = dialog->findChild<QQuickItem *>("propertyText");
    QVERIFY(text);
    QTRY_VERIFY(text->hasActiveFocus());
    auto *propertiesScroll = dialog->findChild<QQuickItem *>("overlayScroll");
    QVERIFY(propertiesScroll);
    key(Qt::Key_PageDown);
    QVERIFY(propertiesScroll->property("contentY").toReal() > 0);
    QVERIFY(text->hasActiveFocus());
    key(Qt::Key_Home, Qt::ControlModifier);
    QTRY_VERIFY(propertiesScroll->property("contentY").toReal() < 1);
    dragOverlayContent(window(), propertiesScroll);
    QTRY_VERIFY(propertiesScroll->property("contentY").toReal() > 0);
    key(Qt::Key_End, Qt::ControlModifier);
    QCOMPARE(text->property("cursorPosition").toInt(), text->property("text").toString().size());
    const QRectF cursor = text->property("cursorRectangle").toRectF();
    const QPointF cursorPoint = text->mapToItem(propertiesScroll, cursor.bottomLeft());
    QVERIFY(cursorPoint.y() <= propertiesScroll->height() + 1);
    key(Qt::Key_Home, Qt::ControlModifier);
    propertiesScroll->setProperty("contentY", 0);
    QMetaObject::invokeMethod(propertiesScroll, "cancelFlick");
    const QPoint selectionStart = text->mapToScene(QPointF(4, 8)).toPoint();
    QTest::mousePress(window(), Qt::LeftButton, Qt::NoModifier, selectionStart);
    QTest::mouseMove(window(), selectionStart + QPoint(140, 0), 30);
    QTest::mouseRelease(window(), Qt::LeftButton, Qt::NoModifier, selectionStart + QPoint(140, 0));
    QVERIFY(!text->property("selectedText").toString().isEmpty());
    key(Qt::Key_A, Qt::ControlModifier);
    QCOMPARE(text->property("selectedText").toString(), text->property("text").toString());
    key(Qt::Key_Escape);
    QVERIFY(!dialog || !dialog->property("visible").toBool());
}

#include "tst_studio.moc"

void TestStudio::keepsSidebarSelectionVisibleInSmallWindows()
{
    const QSize original = window()->size();
    const auto restore = qScopeGuard([&] { window()->resize(original); });
    QStringList vocabulary;
    for (int i = 0; i < 30; ++i)
        vocabulary.append(QStringLiteral("tag%1").arg(i, 2, 10, QLatin1Char('0')));
    putIndexRow(m_tree.filePath("one.txt"), vocabulary);
    m_index->reload();
    settle();
    QFile savedViews(m_desktop.filePath("config/omafiles/views.jsonl"));
    QVERIFY(savedViews.open(QIODevice::ReadOnly));
    const QByteArray originalViews = savedViews.readAll();
    savedViews.close();
    QVERIFY(savedViews.open(QIODevice::Append));
    for (int i = 0; i < 12; ++i)
        savedViews.write(QStringLiteral("{\"name\":\"resize-view-%1\",\"tags\":[\"work\"]}\n").arg(i).toUtf8());
    savedViews.close();
    m_views->reload();
    const auto restoreFixture = qScopeGuard([&] {
        writeIndex(); m_index->reload();
        if (savedViews.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            savedViews.write(originalViews); savedViews.close();
        }
        m_views->reload();
    });
    settle();
    focusRow("one.txt");
    key(Qt::Key_Space);
    window()->resize(480, 320);
    key('t');
    auto *dock = qobject_cast<QQuickItem *>(window()->property("activeDock").value<QObject *>());
    QVERIFY(dock);
    auto *scroll = dock->findChild<QQuickItem *>("dockScroll");
    QVERIFY(scroll);
    auto *list = dock->property("tagsList").value<QQuickItem *>();
    auto *heading = dock->findChild<QQuickItem *>("tagsHeading");
    auto *add = dock->findChild<QQuickItem *>("editTagsButton");
    QVERIFY(list && heading && add);
    const auto visible = [&](QQuickItem *item) {
        if (!item) return false;
        for (auto *ancestor = item->parentItem(); ancestor; ancestor = ancestor->parentItem()) {
            if (ancestor->clip() || ancestor == scroll) {
                const qreal top = item->mapToItem(ancestor, QPointF()).y();
                if (top < -0.5 || top + item->height() > ancestor->height() + 0.5)
                    return false;
            }
            if (ancestor == scroll)
                return true;
        }
        return false;
    };
    QTRY_VERIFY(visible(heading));
    QVERIFY(visible(add));
    for (auto code : {Qt::Key_End, Qt::Key_Home, Qt::Key_PageDown, Qt::Key_PageUp}) {
        key(code);
        QTRY_VERIFY(visible(list->property("currentItem").value<QQuickItem *>()));
        QVERIFY(visible(heading));
    }
    window()->resize(360, 240);
    settle();
    QTRY_VERIFY(visible(list->property("currentItem").value<QQuickItem *>()));
    QVERIFY(visible(heading));
    QTest::qWait(400);
    QVERIFY(dock->isVisible());
    scroll->setProperty("contentY", 0);
    const QPoint point = scroll->mapToScene(QPointF(20, 30)).toPoint();
    QWheelEvent wheel(point, window()->mapToGlobal(point), {}, QPoint(0, -1200),
        Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
    QCoreApplication::sendEvent(window(), &wheel);
    QTRY_VERIFY(scroll->property("contentY").toReal() > 0);
    scroll->setProperty("contentY", 0);
    QMetaObject::invokeMethod(scroll, "cancelFlick");
    QTest::qWait(30);
    QCOMPARE(scroll->property("contentY").toReal(), 0);
    verbRuns().clear();
    dragOverlayContent(window(), scroll);
    QTRY_VERIFY(scroll->property("contentY").toReal() > 0);
    QVERIFY(verbRuns().isEmpty());
    key(Qt::Key_Home);
    QTRY_VERIFY(visible(list->property("currentItem").value<QQuickItem *>()));
    key(Qt::Key_Escape);
    key('v');
    auto *views = dock->property("viewsList").value<QQuickItem *>();
    QVERIFY(views);
    key(Qt::Key_End);
    QTRY_VERIFY(visible(views->property("currentItem").value<QQuickItem *>()));
    window()->resize(320, 180);
    settle();
    QTRY_VERIFY(visible(views->property("currentItem").value<QQuickItem *>()));
    key(Qt::Key_Escape);
    add->forceActiveFocus();
    settle();
    key(Qt::Key_End);
    QVERIFY(scroll->hasActiveFocus());
    key(Qt::Key_Home);
    QCOMPARE(scroll->property("contentY").toReal(), 0);
    key(Qt::Key_Escape);
    auto *folder = qobject_cast<FolderModel *>(filesRoot()->property("folder").value<QObject *>());
    folder->clearMarks();
}

void TestStudio::scrollsActualSheetsWithPageKeys()
{
    const QSize original = window()->size();
    const auto restore = qScopeGuard([&] { window()->resize(original); });
    window()->resize(480, 320);
    key('?');
    auto *help = itemsNamed("helpSheet").first();
    auto *scroll = help->findChild<QQuickItem *>("overlayScroll");
    QVERIFY(help->isVisible());
    key(Qt::Key_PageDown);
    QVERIFY(help->isVisible());
    QTRY_VERIFY(scroll->property("contentY").toReal() > 0);
    key(Qt::Key_End);
    const qreal bottom = scroll->property("contentY").toReal();
    key(Qt::Key_PageUp);
    QVERIFY(scroll->property("contentY").toReal() < bottom);
    key(Qt::Key_Home);
    QCOMPARE(scroll->property("contentY").toReal(), 0);
    QKeyEvent unknown(QEvent::KeyPress, Qt::Key_unknown, Qt::NoModifier);
    QCoreApplication::sendEvent(window(), &unknown);
    QVERIFY(help->isVisible());
    key(Qt::Key_Escape);
    focusRow("one.txt"); key('t'); key('a');
    auto *sheet = itemsNamed("tagSheet").first();
    scroll = sheet->findChild<QQuickItem *>("overlayScroll");
    window()->resize(320, 180);
    settle();
    auto *source = itemsNamed("tagSource").first();
    source->setProperty("candidates", QStringList{"tag00", "tag01", "tag02", "tag03", "tag04", "tag05"});
    source->setProperty("text", "tag");
    settle();
    key(Qt::Key_PageDown);
    QVERIFY(sheet->isVisible());
    QTRY_VERIFY(scroll->property("contentY").toReal() > 0);
    QVERIFY(scroll->hasActiveFocus());
    key(Qt::Key_End);
    QTRY_VERIFY(itemsNamed("tagClose").first()->mapToItem(scroll, QPointF()).y() >= 0);
    key(Qt::Key_Home);
    QCOMPARE(scroll->property("contentY").toReal(), 0);
    key(Qt::Key_Escape);
}

void TestStudio::keepsFileSelectionVisibleAfterResize()
{
    const QSize original = window()->size();
    const auto restore = qScopeGuard([&] { window()->resize(original); tab()->setViewMode("list"); });
    for (const auto &mode : {QStringLiteral("list"), QStringLiteral("details"), QStringLiteral("icons")}) {
        window()->resize(1180, 720);
        tab()->setViewMode(mode);
        settle();
        key(Qt::Key_End);
        auto *view = filesRoot()->property("view").value<QQuickItem *>();
        const int selected = view->property("currentIndex").toInt();
        window()->resize(480, 320);
        settle();
        QCOMPARE(view->property("currentIndex").toInt(), selected);
        auto *current = view->property("currentItem").value<QQuickItem *>();
        QVERIFY(current);
        QTRY_VERIFY(current->mapToItem(view, QPointF()).y() >= -0.5);
        QTRY_VERIFY(current->mapToItem(view, QPointF(0, current->height())).y() <= view->height() + 0.5);
    }
}

namespace {
class JournalDriver
{
public:
    JournalDriver() : path(qgetenv("PATH")), previous(setVerbRunner({}))
    {
        qputenv("PATH", QDir(QCoreApplication::applicationDirPath()).absoluteFilePath("../../build/bin").toUtf8() + ':' + path);
    }
    ~JournalDriver() { setVerbRunner(previous); qputenv("PATH", path); }
    VerbReceipt receipt() {
        return [this](bool success, const QString &, const QString &text) {
            ok = success; notice = text; finished = true;
        };
    }
    void run(const QVector<QStringList> &commands) {
        finished = false;
        journal.run(commands, QStringLiteral("operation"), receipt());
    }
    void undo() { finished = false; journal.undo(receipt()); }
    UndoJournal journal;
    bool finished = false;
    bool ok = false;
    QString notice;
private:
    QByteArray path;
    VerbRunner previous;
};
}

void TestStudio::undoesCompletedFileOperations()
{
    QTemporaryDir fixture;
    QVERIFY(fixture.isValid());
    const QDir dir(fixture.path());
    const QString source = dir.filePath(QStringLiteral("original.txt"));
    const QString moved = dir.filePath(QStringLiteral("renamed.txt"));
    const QString copy = dir.filePath(QStringLiteral("copy.txt"));
    QVERIFY(writeLooknfeel(source, QStringLiteral("original data")));
    putIndexRow(source, {QStringLiteral("work")});
    QString why;
    QVERIFY(writeTagsXattr(source, {QStringLiteral("work")}, &why));
    JournalDriver driver;
    UndoJournal &journal = driver.journal;
    bool &finished = driver.finished;
    bool &ok = driver.ok;
    QString &receipt = driver.notice;
    const auto done = driver.receipt();
    journal.run({{QStringLiteral("mv"), QStringLiteral("--"), source, moved}}, QStringLiteral("rename"), done);
    QTRY_VERIFY(finished);
    QVERIFY2(ok, qPrintable(receipt));
    QVERIFY(!QFileInfo::exists(source));
    QVERIFY(journal.available());
    finished = false;
    journal.run({{QStringLiteral("cp"), QStringLiteral("--"), moved, copy}}, QStringLiteral("copy"), done);
    QTRY_VERIFY(finished);
    QVERIFY2(ok, qPrintable(receipt));
    QVERIFY(QFileInfo::exists(copy));
    finished = false;
    journal.undo(done);
    QTRY_VERIFY(finished);
    QVERIFY2(ok, qPrintable(receipt));
    QVERIFY(!QFileInfo::exists(copy));
    finished = false;
    journal.undo(done);
    QTRY_VERIFY(finished);
    QVERIFY2(ok, qPrintable(receipt));
    QVERIFY(QFileInfo::exists(source));
    QVERIFY(!QFileInfo::exists(moved));
    Store store(defaultIndexPath());
    QVERIFY(store.load(&why));
    QCOMPARE(store.get(source).tags, QStringList{QStringLiteral("work")});
    QStringList tags;
    QVERIFY(readTagsXattr(source, &tags, &why));
    QCOMPARE(tags, QStringList{QStringLiteral("work")});
    QVERIFY(!journal.available());
    const QString folder = dir.filePath(QStringLiteral("new folder"));
    QVERIFY(QDir().mkdir(folder));
    journal.folderCreated(folder);
    QTRY_VERIFY(!journal.busy());
    finished = false;
    journal.undo(done);
    QTRY_VERIFY(finished);
    QVERIFY2(ok, qPrintable(receipt));
    QVERIFY(!QFileInfo::exists(folder));

    finished = false;
    journal.run({{QStringLiteral("--yes"), QStringLiteral("trash"), QStringLiteral("--"), source}}, QStringLiteral("trash"), done);
    QTRY_VERIFY(finished);
    QVERIFY2(ok, qPrintable(receipt));
    QVERIFY(!QFileInfo::exists(source));
    finished = false;
    journal.undo(done);
    QTRY_VERIFY(finished);
    QVERIFY2(ok, qPrintable(receipt));
    QVERIFY(QFileInfo::exists(source));
    QVERIFY(store.load(&why));
    QCOMPARE(store.get(source).tags, QStringList{QStringLiteral("work")});
}

void TestStudio::refusesUndoOfChangedTargets()
{
    QTemporaryDir fixture;
    QVERIFY(fixture.isValid());
    const QDir dir(fixture.path());
    const QString source = dir.filePath(QStringLiteral("source"));
    const QString destination = dir.filePath(QStringLiteral("destination"));
    QVERIFY(writeLooknfeel(source, QStringLiteral("original")));
    JournalDriver driver;
    UndoJournal &journal = driver.journal;
    bool &finished = driver.finished;
    bool &ok = driver.ok;
    QString &receipt = driver.notice;
    const auto done = driver.receipt();
    journal.run({{QStringLiteral("cp"), QStringLiteral("--"), source, destination}}, QStringLiteral("copy"), done);
    QTRY_VERIFY(finished);
    QVERIFY2(ok, qPrintable(receipt));
    struct stat st {};
    QVERIFY(::stat(QFile::encodeName(destination).constData(), &st) == 0);
    QVERIFY(writeLooknfeel(destination, QStringLiteral("modified")));
    const timespec times[]{st.st_atim, st.st_mtim};
    QVERIFY(::utimensat(AT_FDCWD, QFile::encodeName(destination).constData(), times, 0) == 0);
    finished = false;
    journal.undo(done);
    QTRY_VERIFY(finished);
    QVERIFY(!ok);
    QVERIFY2(receipt.contains(QStringLiteral("changed")), qPrintable(receipt));
    QVERIFY(QFileInfo::exists(destination));
    QVERIFY(journal.available());
    QVERIFY(QFile::remove(destination));
    QVERIFY(::symlink(QFile::encodeName(source).constData(), QFile::encodeName(destination).constData()) == 0);
    finished = false;
    journal.undo(done);
    QTRY_VERIFY(finished);
    QVERIFY(!ok);
    QVERIFY(QFileInfo(destination).isSymLink());
    QFile file(source);
    QVERIFY(file.open(QIODevice::ReadOnly));
    QCOMPARE(file.readAll(), QByteArrayLiteral("original"));
    const QString folder = dir.filePath(QStringLiteral("folder"));
    QVERIFY(QDir().mkdir(folder));
    UndoJournal folders;
    folders.folderCreated(folder);
    QTRY_VERIFY(!folders.busy());
    QVERIFY(writeLooknfeel(QDir(folder).filePath(QStringLiteral("child")), QStringLiteral("mine")));
    finished = false;
    folders.undo(done);
    QTRY_VERIFY(finished);
    QVERIFY(!ok);
    QVERIFY2(receipt.contains(QStringLiteral("empty")), qPrintable(receipt));
    QVERIFY(QFileInfo::exists(folder));
}

void TestStudio::undoesTagEditsAndPartialBatches()
{
    QTemporaryDir fixture;
    QVERIFY(fixture.isValid());
    const QDir dir(fixture.path());
    const QString source = dir.filePath(QStringLiteral("source"));
    const QString destination = dir.filePath(QStringLiteral("destination"));
    const QString occupied = dir.filePath(QStringLiteral("occupied"));
    QVERIFY(writeLooknfeel(source, QStringLiteral("source")));
    QVERIFY(writeLooknfeel(occupied, QStringLiteral("mine")));
    JournalDriver driver;
    UndoJournal &journal = driver.journal;
    bool &finished = driver.finished;
    bool &ok = driver.ok;
    QString &receipt = driver.notice;
    const auto done = driver.receipt();
    journal.run({{QStringLiteral("tag"), QStringLiteral("add"), QStringLiteral("--tag"), QStringLiteral("work"), QStringLiteral("--"), source}}, QStringLiteral("tag edit"), done);
    QTRY_VERIFY(finished);
    QVERIFY2(ok, qPrintable(receipt));
    Store store(defaultIndexPath());
    QString why;
    QVERIFY(store.load(&why));
    QCOMPARE(store.get(source).tags, QStringList{QStringLiteral("work")});
    finished = false;
    journal.undo(done);
    QTRY_VERIFY(finished);
    QVERIFY2(ok, qPrintable(receipt));
    QVERIFY(store.load(&why));
    QVERIFY(!store.contains(source));
    QStringList tags;
    QVERIFY(readTagsXattr(source, &tags, &why));
    QVERIFY(tags.isEmpty());
    finished = false;
    journal.run({{QStringLiteral("cp"), QStringLiteral("--"), source, destination},
                 {QStringLiteral("cp"), QStringLiteral("--"), source, occupied}}, QStringLiteral("copy"), done);
    QTRY_VERIFY(finished);
    QVERIFY(!ok);
    QVERIFY(QFileInfo::exists(destination));
    finished = false;
    journal.undo(done);
    QTRY_VERIFY(finished);
    QVERIFY2(ok, qPrintable(receipt));
    QVERIFY(!QFileInfo::exists(destination));
    QVERIFY(QFileInfo::exists(occupied));
    QVERIFY(!journal.available());
}

void TestStudio::offersUndoByKeyboardPaletteAndPointer()
{
    QTemporaryDir fixture;
    QVERIFY(fixture.isValid());
    const QDir dir(fixture.path());
    QObject *root = m_engine->rootObjects().first();
    auto *button = root->findChild<QQuickItem *>(QStringLiteral("undoButton"));
    QVERIFY(button);
    const auto created = [&](const QString &name) {
        return m_index->makeFolder(dir.absolutePath(), name);
    };
    QVERIFY(created(QStringLiteral("keyboard")));
    QTRY_VERIFY(m_index->canUndo());
    key(Qt::Key_Z, Qt::ControlModifier);
    QTRY_VERIFY(!QFileInfo::exists(dir.filePath(QStringLiteral("keyboard"))));
    QVERIFY(created(QStringLiteral("palette")));
    QTRY_VERIFY(m_index->canUndo());
    QVERIFY(QMetaObject::invokeMethod(root->findChild<QObject *>(QStringLiteral("commandPalette")),
        "chosen", Q_ARG(QString, QStringLiteral("undo"))));
    QTRY_VERIFY(!QFileInfo::exists(dir.filePath(QStringLiteral("palette"))));
    QVERIFY(created(QStringLiteral("pointer")));
    QTRY_VERIFY(m_index->canUndo());
    QTest::mouseClick(window(), Qt::LeftButton, Qt::NoModifier,
        button->mapToScene(QPointF(button->width() / 2, button->height() / 2)).toPoint());
    QTRY_VERIFY(!QFileInfo::exists(dir.filePath(QStringLiteral("pointer"))));
}

void TestStudio::undoesEveryTagEdit_data()
{
    QTest::addColumn<QString>("edit");
    for (const char *edit : {"add", "rm", "clear", "cp", "rename", "through-link"})
        QTest::newRow(edit) << QString::fromLatin1(edit);
}

void TestStudio::undoesEveryTagEdit()
{
    QFETCH(QString, edit);
    QTemporaryDir fixture;
    QVERIFY(fixture.isValid());
    const QDir dir(fixture.path());
    const QString file = dir.filePath(QStringLiteral("file"));
    const QString source = dir.filePath(QStringLiteral("tag-source"));
    QVERIFY(writeLooknfeel(file, QStringLiteral("file")));
    QVERIFY(writeLooknfeel(source, QStringLiteral("source")));
    QString why;
    QVERIFY(writeTagsXattr(file, {QStringLiteral("before")}, &why));
    QVERIFY(writeTagsXattr(source, {QStringLiteral("after")}, &why));
    putIndexRow(file, {QStringLiteral("before")});
    Store store(defaultIndexPath());
    QVERIFY(store.load(&why));
    const Record before = store.get(file);
    const QString link = dir.filePath(QStringLiteral("link"));
    QVERIFY(::symlink(QFile::encodeName(file).constData(), QFile::encodeName(link).constData()) == 0);
    QStringList command{QStringLiteral("tag"), edit == QLatin1String("through-link") ? QStringLiteral("add") : edit};
    if (edit == QLatin1String("cp")) command += {QStringLiteral("--from"), source};
    else if (edit == QLatin1String("add") || edit == QLatin1String("through-link")) command += {QStringLiteral("--tag"), QStringLiteral("after")};
    else if (edit == QLatin1String("rm")) command += {QStringLiteral("--tag"), QStringLiteral("before")};
    command += QStringLiteral("--");
    if (edit == QLatin1String("rename")) command += {QStringLiteral("before"), QStringLiteral("after")};
    command += edit == QLatin1String("through-link") ? link : file;
    JournalDriver driver;
    driver.run({command});
    QTRY_VERIFY(driver.finished);
    QVERIFY2(driver.ok, qPrintable(driver.notice));
    QVERIFY(driver.journal.available());
    driver.undo();
    QTRY_VERIFY(driver.finished);
    QVERIFY2(driver.ok, qPrintable(driver.notice));
    QVERIFY(store.load(&why));
    QCOMPARE(store.get(file), before);
    QStringList tags;
    QVERIFY(readTagsXattr(file, &tags, &why));
    QCOMPARE(tags, QStringList{QStringLiteral("before")});
    QVERIFY(QFileInfo(link).isSymLink());
    QVERIFY(!driver.journal.available());
}

void TestStudio::undoesDirectoryAndSymbolicLinkMoves()
{
    QTemporaryDir fixture;
    QVERIFY(fixture.isValid());
    const QDir dir(fixture.path());
    const QString folder = dir.filePath(QStringLiteral("folder"));
    const QString child = QDir(folder).filePath(QStringLiteral("child"));
    const QString renamed = dir.filePath(QStringLiteral("folder-new"));
    QVERIFY(QDir().mkdir(folder));
    QVERIFY(writeLooknfeel(child, QStringLiteral("nested data")));
    putIndexRow(child, {QStringLiteral("nested")});
    QString why;
    QVERIFY(writeTagsXattr(child, {QStringLiteral("nested")}, &why));
    JournalDriver driver;
    driver.run({{QStringLiteral("mv"), QStringLiteral("--pattern"), QStringLiteral("{name}-new"), QStringLiteral("--"), folder, child}});
    QTRY_VERIFY(driver.finished);
    QVERIFY2(driver.ok, qPrintable(driver.notice));
    QVERIFY(QFileInfo::exists(QDir(renamed).filePath(QStringLiteral("child-new"))));
    driver.undo();
    QTRY_VERIFY(driver.finished);
    QVERIFY2(driver.ok, qPrintable(driver.notice));
    QVERIFY(QFileInfo::exists(child));
    QVERIFY(!QFileInfo::exists(renamed));
    Store store(defaultIndexPath());
    QVERIFY(store.load(&why));
    QCOMPARE(store.get(child).tags, QStringList{QStringLiteral("nested")});
    const QString link = dir.filePath(QStringLiteral("link"));
    const QString moved = dir.filePath(QStringLiteral("link-new"));
    QVERIFY(::symlink("folder/child", QFile::encodeName(link).constData()) == 0);
    driver.run({{QStringLiteral("mv"), QStringLiteral("--"), link, moved}});
    QTRY_VERIFY(driver.finished);
    QVERIFY2(driver.ok, qPrintable(driver.notice));
    driver.undo();
    QTRY_VERIFY(driver.finished);
    QVERIFY2(driver.ok, qPrintable(driver.notice));
    QCOMPARE(linkTarget(QFile::encodeName(link)), QByteArrayLiteral("folder/child"));
    QVERIFY(!QFileInfo::exists(moved));
}

void TestStudio::refusesUndoAfterIndependentTagAndModeChanges()
{
    QTemporaryDir fixture;
    QVERIFY(fixture.isValid());
    const QDir dir(fixture.path());
    const QString file = dir.filePath(QStringLiteral("file"));
    const QString moved = dir.filePath(QStringLiteral("moved"));
    QVERIFY(writeLooknfeel(file, QStringLiteral("file")));
    JournalDriver driver;
    driver.run({{QStringLiteral("mv"), QStringLiteral("--"), file, moved}});
    QTRY_VERIFY(driver.finished);
    QVERIFY2(driver.ok, qPrintable(driver.notice));
    QVERIFY(::symlink("missing", QFile::encodeName(file).constData()) == 0);
    driver.undo();
    QTRY_VERIFY(driver.finished);
    QVERIFY(!driver.ok);
    QVERIFY2(driver.notice.contains(QStringLiteral("occupied")), qPrintable(driver.notice));
    QVERIFY(QFileInfo(file).isSymLink());
    QVERIFY(QFile::remove(file));
    struct stat st {};
    QVERIFY(::stat(QFile::encodeName(moved).constData(), &st) == 0);
    QVERIFY(::chmod(QFile::encodeName(moved).constData(), (st.st_mode & 07777) ^ S_IXUSR) == 0);
    driver.undo();
    QTRY_VERIFY(driver.finished);
    QVERIFY(!driver.ok);
    QVERIFY(::chmod(QFile::encodeName(moved).constData(), st.st_mode & 07777) == 0);
    putIndexRow(moved, {QStringLiteral("independent")});
    driver.undo();
    QTRY_VERIFY(driver.finished);
    QVERIFY(!driver.ok);
    QVERIFY2(driver.notice.contains(QStringLiteral("indexed tags changed")), qPrintable(driver.notice));
    Store store(defaultIndexPath());
    QString why;
    QVERIFY(store.lockAndLoad(&why));
    store.remove(moved);
    QVERIFY(store.commit(&why));
    QVERIFY(writeTagsXattr(moved, {QStringLiteral("independent")}, &why));
    driver.undo();
    QTRY_VERIFY(driver.finished);
    QVERIFY(!driver.ok);
    QVERIFY(writeTagsXattr(moved, {}, &why));
    driver.undo();
    QTRY_VERIFY(driver.finished);
    QVERIFY2(driver.ok, qPrintable(driver.notice));
    QVERIFY(QFileInfo::exists(file));
    QVERIFY(!QFileInfo::exists(moved));
}

void TestStudio::boundsSessionUndoHistory()
{
    QTemporaryDir fixture;
    QVERIFY(fixture.isValid());
    JournalDriver driver;
    for (int at = 0; at < 51; ++at) {
        const QString path = fixture.filePath(QString::number(at));
        QVERIFY(QDir().mkdir(path));
        driver.journal.folderCreated(path);
        QTRY_VERIFY(!driver.journal.busy());
    }
    for (int at = 50; at > 0; --at) {
        driver.undo();
        QTRY_VERIFY(driver.finished);
        QVERIFY2(driver.ok, qPrintable(driver.notice));
        QVERIFY(!QFileInfo::exists(fixture.filePath(QString::number(at))));
    }
    QVERIFY(QFileInfo::exists(fixture.filePath(QStringLiteral("0"))));
    QVERIFY(!driver.journal.available());
    UndoJournal newSession;
    QVERIFY(!newSession.available());
}

void TestStudio::refusesUndoWhenTheIndexCannotCommit()
{
    QTemporaryDir fixture;
    QVERIFY(fixture.isValid());
    const QString path = fixture.filePath(QStringLiteral("file"));
    QVERIFY(writeLooknfeel(path, QStringLiteral("file")));
    JournalDriver driver;
    driver.run({{QStringLiteral("tag"), QStringLiteral("add"), QStringLiteral("--tag"), QStringLiteral("work"), QStringLiteral("--"), path}});
    QTRY_VERIFY(driver.finished);
    QVERIFY2(driver.ok, qPrintable(driver.notice));
    QFile index(defaultIndexPath());
    const auto permissions = index.permissions();
    const auto restore = qScopeGuard([&] { index.setPermissions(permissions); });
    QVERIFY(index.setPermissions(QFileDevice::ReadOwner));
    driver.undo();
    QTRY_VERIFY(driver.finished);
    QVERIFY(!driver.ok);
    QString why;
    QStringList tags;
    QVERIFY(readTagsXattr(path, &tags, &why));
    QCOMPARE(tags, QStringList{QStringLiteral("work")});
    Store store(defaultIndexPath());
    QVERIFY(store.load(&why));
    QCOMPARE(store.get(path).tags, QStringList{QStringLiteral("work")});
    QVERIFY(index.setPermissions(permissions));
    driver.undo();
    QTRY_VERIFY(driver.finished);
    QVERIFY2(driver.ok, qPrintable(driver.notice));
    QVERIFY(store.load(&why));
    QVERIFY(!store.contains(path));
    tags.clear();
    QVERIFY(readTagsXattr(path, &tags, &why));
    QVERIFY(tags.isEmpty());
}

void TestStudio::refusesUndoOfAnIdenticalReplacement()
{
    QTemporaryDir fixture;
    QVERIFY(fixture.isValid());
    const QString source = fixture.filePath(QStringLiteral("source"));
    const QString destination = fixture.filePath(QStringLiteral("destination"));
    QVERIFY(writeLooknfeel(source, QStringLiteral("identical")));
    JournalDriver driver;
    driver.run({{QStringLiteral("cp"), QStringLiteral("--"), source, destination}});
    QTRY_VERIFY(driver.finished);
    QVERIFY2(driver.ok, qPrintable(driver.notice));
    QFile held(destination);
    QVERIFY(held.open(QIODevice::ReadOnly));
    struct stat st {};
    QVERIFY(::fstat(held.handle(), &st) == 0);
    QVERIFY(QFile::remove(destination));
    QVERIFY(QFile::copy(source, destination));
    const timespec times[]{st.st_atim, st.st_mtim};
    QVERIFY(::utimensat(AT_FDCWD, QFile::encodeName(destination).constData(), times, 0) == 0);
    driver.undo();
    QTRY_VERIFY(driver.finished);
    QVERIFY(!driver.ok);
    QVERIFY2(driver.notice.contains(QStringLiteral("changed")), qPrintable(driver.notice));
    QVERIFY(QFileInfo::exists(destination));
}

void TestStudio::preservesSuccessfulProcessDiagnostics()
{
    QTemporaryDir helper;
    QVERIFY(helper.isValid());
    QFile script(helper.filePath("omafiles"));
    QVERIFY(script.open(QIODevice::WriteOnly));
    script.write("#!/bin/sh\nprintf 'committed\\n'\nprintf 'mirror note\\nrepair hint\\n' >&2\n");
    script.close();
    QVERIFY(script.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
    const QByteArray originalPath = qgetenv("PATH");
    qputenv("PATH", helper.path().toUtf8() + ':' + originalPath);
    const auto previous = setVerbRunner({});
    const auto restore = qScopeGuard([&] { setVerbRunner(previous); qputenv("PATH", originalPath); });
    QCOMPARE(verbProgram(), script.fileName());
    bool finished = false, succeeded = false;
    QString stdoutText, diagnostics;
    runVerb({"tag", "add"}, [&](bool ok, const QString &output, const QString &notes) {
        succeeded = ok; stdoutText = output; diagnostics = notes; finished = true;
    });
    QTRY_VERIFY(finished);
    QVERIFY(succeeded);
    QCOMPARE(stdoutText, QStringLiteral("committed\n"));
    QCOMPARE(diagnostics, QStringLiteral("mirror note\nrepair hint"));
}

void TestStudio::keepsCliNotesInWindowReceipts()
{
    int run = 0;
    const auto previous = setVerbRunner([&](const QStringList &, VerbReceipt done) {
        done(true, {}, QStringLiteral("mirror note %1; use tag clear").arg(++run));
    });
    const auto restore = qScopeGuard([&] { setVerbRunner(previous); m_views->reload(); });
    focusRow("plain.txt"); key('t'); key('a');
    auto *source = itemsNamed("tagSource").first();
    source->setProperty("text", "new");
    key(Qt::Key_Return);
    QVERIFY(!itemsNamed("tagSheet").first()->isVisible());
    QVERIFY(m_index->notice().contains("tagged 1 file new"));
    QVERIFY(m_index->notice().contains("mirror note 1; use tag clear"));
    QCOMPARE(itemsNamed("statusBar").first()->property("message").toString(), m_index->notice());
    auto *receipt = itemsNamed("statusReceipt").first();
    for (int i = 0; i < 100 && !receipt->hasActiveFocus(); ++i)
        key(Qt::Key_Tab);
    QVERIFY(receipt->hasActiveFocus());
    QCOMPARE(receipt->property("color").value<QColor>().alpha(), 0);
    key(Qt::Key_Return);
    QPointer<QObject> dialog = window()->findChild<QObject *>("propertiesDialog");
    auto *text = dialog->findChild<QQuickItem *>("propertyText");
    QTRY_VERIFY(text->hasActiveFocus());
    QCOMPARE(text->property("text").toString(), m_index->notice());
    const QSize originalSize = window()->size();
    const auto restoreSize = qScopeGuard([&] { window()->resize(originalSize); });
    window()->resize(320, 180);
    settle();
    auto *viewport = dialog->findChild<QQuickItem *>("overlayScroll");
    QVERIFY(viewport);
    const QFont font = text->property("font").value<QFont>();
    QVERIFY(viewport->height() >= 2 * QFontMetricsF(font).lineSpacing());
    for (const auto code : {Qt::Key_Home, Qt::Key_End}) {
        key(code, Qt::ControlModifier);
        const QRectF cursor = text->property("cursorRectangle").toRectF();
        const QPointF top = text->mapToItem(viewport, cursor.topLeft());
        QTRY_VERIFY(top.y() >= -0.5);
        QVERIFY(top.y() + cursor.height() <= viewport->height() + 0.5);
    }
    window()->resize(originalSize);
    settle();
    key(Qt::Key_A, Qt::ControlModifier);
    key(Qt::Key_C, Qt::ControlModifier);
    QCOMPARE(QGuiApplication::clipboard()->text(), m_index->notice());
    key(Qt::Key_Escape);
    const QPoint point = receipt->mapToScene(QPointF(receipt->width() / 2, receipt->height() / 2)).toPoint();
    QTest::mouseClick(window(), Qt::LeftButton, Qt::NoModifier, point);
    dialog = window()->findChild<QObject *>("propertiesDialog");
    QVERIFY(dialog);
    text = dialog->findChild<QQuickItem *>("propertyText");
    QVERIFY(text);
    QTRY_VERIFY(text->hasActiveFocus());
    QCOMPARE(text->property("text").toString(), m_index->notice());
    key(Qt::Key_Escape);
    m_index->copyInto({m_tree.filePath("one.txt"), m_tree.filePath("two.md")}, m_tree.filePath("alpha"));
    QTRY_VERIFY(m_index->notice().contains("copied 2 files"));
    QVERIFY(m_index->notice().contains("mirror note 2"));
    QVERIFY(m_index->notice().contains("mirror note 3"));
    m_index->pruneTree(m_tree.path());
    QVERIFY(m_index->notice().contains("nothing to prune"));
    QVERIFY(m_index->notice().contains("mirror note 4"));
    m_index->verifyTree(m_tree.path());
    QVERIFY(m_index->notice().contains("is clean"));
    QVERIFY(m_index->notice().contains("mirror note 5"));
    m_views->saveQuery("warned view", {"work"});
    QVERIFY(m_index->notice().contains("mirror note 6"));
    QVERIFY(m_views->error().isEmpty());
    QCOMPARE(verbNotice(false, "success", "refusal"), QStringLiteral("refusal"));
}

namespace {
struct ThumbnailFixture {
    QTemporaryDir root;
    QByteArray cache = qgetenv("XDG_CACHE_HOME");
    QByteArray data = qgetenv("XDG_DATA_HOME");
    ThumbnailFixture() {
        qputenv("XDG_CACHE_HOME", root.filePath("cache").toUtf8());
        qputenv("XDG_DATA_HOME", root.filePath("data").toUtf8());
    }
    ~ThumbnailFixture() {
        qputenv("XDG_CACHE_HOME", cache);
        qputenv("XDG_DATA_HOME", data);
    }
    QString cached(const QString &path, const QString &tier = "normal") const {
        return root.filePath("cache/thumbnails/" + tier + '/')
            + QString::fromLatin1(QCryptographicHash::hash(QUrl::fromLocalFile(path).toEncoded(),
                QCryptographicHash::Md5).toHex()) + ".png";
    }
    ThumbnailCancellation token() const { return std::make_shared<std::atomic_bool>(false); }
    QString source(const QString &name, QColor color = Qt::red) const {
        const QString path = root.filePath(name);
        QImage image(512, 256, QImage::Format_ARGB32);
        image.fill(color);
        return image.save(path, "png") ? path : QString();
    }
    QString pdf(const QString &name) const {
        const QString path = root.filePath(name);
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly)) return {};
        file.write("%PDF-1.7\nfixture\n");
        return path;
    }
    void provider() const {
        const QString dir = root.filePath("data/thumbnailers");
        QDir().mkpath(dir);
        QFile file(dir + "/fake.thumbnailer");
        if (file.open(QIODevice::WriteOnly))
            file.write("[Thumbnailer Entry]\nTryExec=/usr/bin/true\nExec=/usr/bin/true %i %u %o %s %% \"literal with spaces\"\nMimeType=application/pdf;video/mp4;\n");
    }
};

bool writeThumbnailControlFile(const QString &path, const QByteArray &bytes)
{
    if (!QDir().mkpath(QFileInfo(path).path())) return false;
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}

bool waitForThumbnailWorkers()
{
    auto *queue = qApp->findChild<QObject *>("thumbnailQueue");
    if (!queue) return true;
    return QTest::qWaitFor([&] {
        for (auto *watcher : queue->findChildren<QFutureWatcherBase *>())
            if (watcher->isRunning()) return false;
        return true;
    }, asyncTimeout);
}
}

void TestStudio::boundsThumbnailPixelsToTheDisplayAndRetainsTheSharedCache()
{
    QTemporaryDir tree;
    const QString path = tree.filePath("display-bounds.png");
    QImage source(512, 256, QImage::Format_RGB32);
    source.fill(Qt::red);
    QVERIFY(source.save(path));
    Thumbnail item(window()->contentItem());
    item.setSize(QSizeF(64, 32));
    item.setExtent(256);
    item.setPath(path);
    item.setRequested(true);
    const auto expected = [&](const QSize &size) {
        const qreal ratio = window()->devicePixelRatio();
        return QSize(qCeil(size.width() * ratio), qCeil(size.height() * ratio))
            .boundedTo(QSize(256, 256));
    };
    QTRY_VERIFY(item.ready());
    QCOMPARE(item.imageSize(), expected(QSize(64, 32)));
    const auto cancel = std::make_shared<std::atomic_bool>(false);
    QCOMPARE(thumbnailFor(path, 256, cancel).size(), QSize(256, 128));
    item.setSize(QSizeF(128, 64));
    QTRY_COMPARE(item.imageSize(), expected(QSize(128, 64)));
    item.setSize(QSizeF(16, 8));
    QTRY_COMPARE(item.imageSize(), expected(QSize(16, 8)));
    item.setRequested(false);
    item.setSize(QSizeF(32, 16));
    item.setRequested(true);
    QTRY_COMPARE(item.imageSize(), expected(QSize(32, 16)));
    QCOMPARE(thumbnailFor(path, 256, cancel).size(), QSize(256, 128));
}

void TestStudio::avoidsSourceDecodersForEmptyFiles()
{
    ThumbnailFixture f;
    QVERIFY(QDir().mkpath(f.root.filePath("data/thumbnailers")));
    QFile ignored(f.root.filePath("data/thumbnailers/ignored.thumbnailer"));
    QVERIFY(ignored.open(QIODevice::WriteOnly));
    ignored.write("[Thumbnailer Entry]\nExec=/usr/bin/true %i %o\nMimeType=;\n");
    ignored.close();
    for (const QString &name : {QStringLiteral("empty.txt"), QStringLiteral("empty.png")}) {
        QFile file(f.root.filePath(name));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.close();
        ThumbnailWork work;
        QVERIFY(thumbnailFor(file.fileName(), 128, f.token(), false, &work).isNull());
        QCOMPARE(work.sourceDecodes, 0);
        QCOMPARE(work.mimeLookups, 0);
    }
    for (const QString &name : {QStringLiteral("image.txt"), QStringLiteral("extensionless")}) {
        const QString path = f.source(name);
        QVERIFY(!path.isEmpty());
        ThumbnailWork work;
        QCOMPARE(thumbnailFor(path, 128, f.token(), false, &work).pixelColor(10, 10), QColor(Qt::red));
        QCOMPARE(work.sourceDecodes, 1);
        QVERIFY(!thumbnailFor(path, 128, f.token(), false, &work).isNull());
        QCOMPARE(work.sourceDecodes, 0);
    }
    QFile bitmap(f.root.filePath("bitmap.txt"));
    QVERIFY(bitmap.open(QIODevice::WriteOnly));
    bitmap.write("#define test_width 8\n#define test_height 1\nstatic char test_bits[] = {\n 0xff };\n");
    bitmap.close();
    ThumbnailWork work;
    const QImage image = thumbnailFor(bitmap.fileName(), 128, f.token(), false, &work);
    QCOMPARE(image.size(), QSize(8, 1));
    QCOMPARE(work.sourceDecodes, 1);

    QVERIFY(QDir().mkpath(f.root.filePath("data/thumbnailers")));
    QFile provider(f.root.filePath("data/thumbnailers/empty.thumbnailer"));
    QVERIFY(provider.open(QIODevice::WriteOnly));
    provider.write("[Thumbnailer Entry]\nTryExec=/usr/bin/true\nExec=/usr/bin/true %i %o\nMimeType=application/x-zerosize;\n");
    provider.close();
    int calls = 0;
    const auto previous = setThumbnailRunner([&](const QStringList &args, const ThumbnailCancellation &, qint64, int) {
        ++calls;
        QImage supplied(64, 64, QImage::Format_ARGB32); supplied.fill(Qt::green);
        return supplied.save(args.at(2), "png");
    });
    const auto restore = qScopeGuard([&] { setThumbnailRunner(previous); });
    const QString empty = f.root.filePath("extensionless-empty");
    { QFile file(empty); QVERIFY(file.open(QIODevice::WriteOnly)); }
    QCOMPARE(thumbnailFor(empty, 128, f.token(), false, &work).pixelColor(10, 10), QColor(Qt::green));
    QCOMPARE(work.sourceDecodes, 0);
    QCOMPARE(calls, 1);
    QCOMPARE(work.mimeLookups, 1);
}

void TestStudio::sharesValidFreedesktopThumbnails()
{
    ThumbnailFixture f;
    const QString path = f.source("space # percent % café.png");
    QVERIFY(!path.isEmpty());
    const QImage normal = thumbnailFor(path, 128, f.token());
    QCOMPARE(normal.size(), QSize(128, 64));
    QCOMPARE(normal.pixelColor(20, 20), QColor(Qt::red));
    QFile encoded(f.cached(path));
    QVERIFY(encoded.open(QIODevice::ReadOnly));
    QVERIFY(encoded.readAll().contains("tEXtThumb::URI\0"));
    encoded.close();
    QImage saved(f.cached(path));
    QCOMPARE(saved.text("Thumb::URI"), QString::fromUtf8(QUrl::fromLocalFile(path).toEncoded()));
    QCOMPARE(saved.text("Thumb::MTime"), QString::number(QFileInfo(path).lastModified().toSecsSinceEpoch()));
    QCOMPARE(saved.text("Thumb::Size"), QString::number(QFileInfo(path).size()));
    struct stat info {};
    QVERIFY(::stat(QFile::encodeName(f.cached(path)).constData(), &info) == 0);
    QCOMPARE(info.st_mode & 0777, mode_t(0600));
    QVERIFY(::stat(QFile::encodeName(QFileInfo(f.cached(path)).absolutePath()).constData(), &info) == 0);
    QCOMPARE(info.st_mode & 0777, mode_t(0700));
    saved.fill(Qt::green);
    saved.setText("Thumb::Size", {});
    QVERIFY(saved.save(f.cached(path)));
    QCOMPARE(thumbnailFor(path, 128, f.token()).pixelColor(20, 20), QColor(Qt::green));
    QCOMPARE(thumbnailFor(path, 256, f.token()).size(), QSize(256, 128));
    QVERIFY(QFile::remove(f.cached(path)));
    QImage large(f.cached(path, "large"));
    large.fill(Qt::blue);
    QVERIFY(large.save(f.cached(path, "large")));
    QCOMPARE(thumbnailFor(path, 128, f.token()).pixelColor(20, 20), QColor(Qt::blue));
    const QString alpha = f.source("transparent.png", QColor(255, 0, 0, 64));
    QCOMPARE(thumbnailFor(alpha, 128, f.token()).pixelColor(20, 20).alpha(), 64);
    const QString oriented = f.root.filePath("oriented.jpg");
    QImage portrait(512, 256, QImage::Format_RGB32); portrait.fill(Qt::red);
    QVERIFY(portrait.save(oriented, "jpeg"));
    QFile jpeg(oriented); QVERIFY(jpeg.open(QIODevice::ReadOnly));
    QByteArray bytes = jpeg.readAll(); jpeg.close();
    bytes.insert(2, QByteArray::fromHex("ffe1002245786966000049492a0008000000010012010300010000000600000000000000"));
    QVERIFY(jpeg.open(QIODevice::WriteOnly | QIODevice::Truncate));
    QCOMPARE(jpeg.write(bytes), bytes.size()); jpeg.close();
    QCOMPARE(thumbnailFor(oriented, 128, f.token()).size(), QSize(64, 128));
}

void TestStudio::invalidatesChangedOrMalformedThumbnails()
{
    ThumbnailFixture f;
    const QString path = f.source("changed.png");
    QVERIFY(!thumbnailFor(path, 128, f.token()).isNull());
    for (const QString &key : {QStringLiteral("Thumb::URI"), QStringLiteral("Thumb::MTime"), QStringLiteral("Thumb::Size")}) {
        QImage bad(f.cached(path));
        bad.fill(Qt::green);
        bad.setText(key, "wrong");
        QVERIFY(bad.save(f.cached(path)));
        QCOMPARE(thumbnailFor(path, 128, f.token()).pixelColor(20, 20), QColor(Qt::red));
    }
    QFile corrupt(f.cached(path));
    QVERIFY(corrupt.open(QIODevice::WriteOnly | QIODevice::Truncate));
    corrupt.write("not a PNG"); corrupt.close();
    QCOMPARE(thumbnailFor(path, 128, f.token()).pixelColor(20, 20), QColor(Qt::red));
    QCOMPARE(f.source("changed.png", Qt::blue), path);
    QFile source(path);
    QVERIFY(source.open(QIODevice::Append)); source.write("new version"); source.close();
    QCOMPARE(thumbnailFor(path, 128, f.token()).pixelColor(20, 20), QColor(Qt::blue));
    QVERIFY(::chmod(QFile::encodeName(path).constData(), 0000) == 0);
    QVERIFY(thumbnailFor(path, 128, f.token()).isNull());
    QVERIFY(::chmod(QFile::encodeName(path).constData(), 0600) == 0);
    QVERIFY(thumbnailFor(f.root.filePath("missing.png"), 128, f.token()).isNull());
    const QString fifo = f.root.filePath("fifo.png");
    QVERIFY(::mkfifo(QFile::encodeName(fifo).constData(), 0600) == 0);
    QVERIFY(thumbnailFor(fifo, 128, f.token()).isNull());
    QVERIFY(thumbnailFor(f.cached(path), 128, f.token()).isNull());
    const QString alias = f.root.filePath("cache-alias.png");
    QVERIFY(QFile::link(f.cached(path), alias));
    QVERIFY(thumbnailFor(alias, 128, f.token()).isNull());
    const QString broken = f.root.filePath("broken.png");
    QFile brokenFile(broken); QVERIFY(brokenFile.open(QIODevice::WriteOnly));
    brokenFile.write("invalid readable image"); brokenFile.close();
    QVERIFY(thumbnailFor(broken, 128, f.token()).isNull());
    QVERIFY(QFileInfo::exists(f.cached(broken, "fail/omafiles-" + QStringLiteral(OMAFILES_VERSION))));
}

void TestStudio::runsInstalledThumbnailersAndCachesFailures()
{
    ThumbnailFixture f;
    f.provider();
    QStringList seen;
    int calls = 0;
    bool succeeds = true;
    const auto previous = setThumbnailRunner([&](const QStringList &args, const ThumbnailCancellation &, qint64, int) {
        ++calls; seen = args;
        if (!succeeds) {
            QFile output(args.at(3));
            if (!output.open(QIODevice::WriteOnly)) return false;
            output.write("invalid helper output");
            return true;
        }
        QImage image(300, 150, QImage::Format_ARGB32); image.fill(Qt::yellow);
        return image.save(args.at(3), "png");
    });
    const auto restore = qScopeGuard([&] { setThumbnailRunner(previous); });
    const QString path = f.pdf("space # % document.pdf");
    const QImage image = thumbnailFor(path, 128, f.token());
    QVERIFY(!image.isNull());
    QCOMPARE(image.size(), QSize(128, 64));
    QCOMPARE(image.pixelColor(20, 20), QColor(Qt::yellow));
    QCOMPARE(seen.at(1), path);
    QCOMPARE(seen.at(2), QString::fromUtf8(QUrl::fromLocalFile(path).toEncoded()));
    QCOMPARE(seen.at(4), QStringLiteral("128"));
    QCOMPARE(seen.at(5), QStringLiteral("%"));
    QCOMPARE(seen.at(6), QStringLiteral("literal with spaces"));
    QCOMPARE(thumbnailFor(path, 128, f.token()), image);
    QCOMPARE(calls, 1);
    succeeds = false;
    const QString broken = f.pdf("broken.pdf");
    QVERIFY(thumbnailFor(broken, 128, f.token()).isNull());
    QCOMPARE(calls, 2);
    const QString fail = f.cached(broken, "fail/omafiles-" + QStringLiteral(OMAFILES_VERSION));
    QImage marker(fail);
    QCOMPARE(marker.size(), QSize(1, 1));
    QVERIFY(!marker.text("Thumb::URI").isEmpty());
    QVERIFY(thumbnailFor(broken, 128, f.token()).isNull());
    QCOMPARE(calls, 2);
    QFile changed(broken); QVERIFY(changed.open(QIODevice::Append)); changed.write("changed"); changed.close();
    succeeds = true;
    QVERIFY(!thumbnailFor(broken, 128, f.token()).isNull());
    QCOMPARE(calls, 3);
    const QString movie = f.root.filePath("movie.mp4");
    { QFile file(movie); QVERIFY(file.open(QIODevice::WriteOnly));
      file.write(QByteArray::fromHex("00000018667479706d703432000000006d70343269736f6d")); }
    QVERIFY(!thumbnailFor(movie, 256, f.token()).isNull());
    QCOMPARE(calls, 4);
    const auto canceled = f.token(); canceled->store(true);
    QVERIFY(thumbnailFor(f.pdf("canceled.pdf"), 128, canceled).isNull());
    QCOMPARE(calls, 4);
    const QString changing = f.pdf("changing.pdf");
    setThumbnailRunner([&](const QStringList &args, const ThumbnailCancellation &, qint64, int) {
        QFile file(changing); if (file.open(QIODevice::Append)) file.write("changed during generation");
        QImage out(40, 40, QImage::Format_ARGB32); out.fill(Qt::cyan);
        return out.save(args.at(3));
    });
    QVERIFY(thumbnailFor(changing, 128, f.token()).isNull());
    QVERIFY(!QFileInfo::exists(f.cached(changing)));
    QVERIFY(!QFileInfo::exists(f.cached(changing, "fail/omafiles-" + QStringLiteral(OMAFILES_VERSION))));
    QFile descriptor(f.root.filePath("data/thumbnailers/fake.thumbnailer"));
    QVERIFY(descriptor.open(QIODevice::WriteOnly | QIODevice::Truncate));
    descriptor.write("[Thumbnailer Entry]\nTryExec=/missing/thumbnailer\nExec=/usr/bin/true %i %o\nMimeType=video/mp4;\n");
    descriptor.close();
    bool invoked = false;
    setThumbnailRunner([&](const QStringList &, const ThumbnailCancellation &, qint64, int) { invoked = true; return false; });
    const QString unavailable = f.root.filePath("unavailable.mp4");
    QVERIFY(QFile::copy(movie, unavailable));
    QVERIFY(thumbnailFor(unavailable, 128, f.token()).isNull());
    QVERIFY(!invoked);
    QVERIFY(!QFileInfo::exists(f.cached(unavailable, "fail/omafiles-" + QStringLiteral(OMAFILES_VERSION))));
}

void TestStudio::refusesLinkedThumbnailCachePaths()
{
    ThumbnailFixture f;
    const QString path = f.source("safe.png");
    const QString outside = f.root.filePath("outside");
    QVERIFY(QDir().mkpath(outside));
    QVERIFY(QDir().mkpath(f.root.filePath("cache")));
    QVERIFY(QFile::link(outside, f.root.filePath("cache/thumbnails")));
    QVERIFY(!thumbnailFor(path, 128, f.token()).isNull());
    QVERIFY(QDir(outside).entryList(QDir::NoDotAndDotDot | QDir::AllEntries).isEmpty());
    QVERIFY(QFile::remove(f.root.filePath("cache/thumbnails")));
    QVERIFY(QDir().mkpath(f.root.filePath("cache/thumbnails/normal")));
    const QString target = f.source("untouched.png", Qt::green);
    QImage valid = thumbnailFor(path, 128, f.token());
    valid.fill(Qt::green);
    QVERIFY(valid.save(target));
    QFile original(target); QVERIFY(original.open(QIODevice::ReadOnly)); const QByteArray bytes = original.readAll(); original.close();
    QVERIFY(QFile::remove(f.cached(path)));
    QVERIFY(QFile::link(target, f.cached(path)));
    QCOMPARE(thumbnailFor(path, 128, f.token()).pixelColor(20, 20), QColor(Qt::red));
    QVERIFY(QFileInfo(f.cached(path)).isSymLink());
    QVERIFY(original.open(QIODevice::ReadOnly)); QCOMPARE(original.readAll(), bytes);
}

void TestStudio::boundsThumbnailWorkersByCgroupHierarchy_data()
{
    QTest::addColumn<bool>("unified");
    QTest::addColumn<QString>("member");
    QTest::addColumn<QString>("root");
    QTest::addColumn<QString>("leafQuota");
    QTest::addColumn<QString>("parentQuota");
    QTest::addColumn<int>("expected");
    for (bool unified : {true, false}) {
        const QString version = unified ? "v2-" : "v1-";
        const QString unlimited = unified ? "max" : "-1";
        const auto row = [&](const char *name, QString leaf, QString parent, int expected,
                             QString member = "/team/leaf", QString root = "/") {
            QTest::newRow(qPrintable(version + name)) << unified << member << root << leaf << parent << expected;
        };
        row("unlimited", unlimited, unlimited, 4);
        row("one", "100000", unlimited, 1);
        row("two", "200000", unlimited, 2);
        row("fractional", "150000", unlimited, 1);
        row("sub-core", "50000", unlimited, 1);
        row("ancestor-one", "200000", "100000", 1);
        row("ancestor-two", unlimited, "200000", 2);
        row("malformed", "broken", unlimited, 1);
        row("unmapped", unlimited, unlimited, 1, "/else/team/leaf", "/host");
        if (unified) row("invalid-negative", "-1", unlimited, 1);
        row("nested-mount", unlimited, "100000", 1, "/host/team/leaf", "/host");
        row("namespace-root", "200000", unlimited, 2, "/", "/host/container");
    }
}

void TestStudio::boundsThumbnailWorkersByCgroupHierarchy()
{
    QFETCH(bool, unified);
    QFETCH(QString, member);
    QFETCH(QString, root);
    QFETCH(QString, leafQuota);
    QFETCH(QString, parentQuota);
    QFETCH(int, expected);
    cpu_set_t affinity;
    QVERIFY(::sched_getaffinity(0, sizeof(affinity), &affinity) == 0);
    QTemporaryDir fixture;
    QVERIFY(fixture.isValid());
    const QString mountpoint = fixture.filePath("control groups");
    QString escaped = mountpoint;
    escaped.replace(' ', "\\040");
    const QByteArray options = unified ? "cgroup2 cgroup2 rw" : "cgroup cpu rw,cpu,cpuacct";
    QVERIFY(writeThumbnailControlFile(fixture.filePath("self/mountinfo"),
        "32 1 0:30 " + root.toUtf8() + ' ' + escaped.toUtf8() + " rw - " + options + '\n'));
    QVERIFY(writeThumbnailControlFile(fixture.filePath("self/cgroup"),
        (unified ? QByteArray("0::") : QByteArray("3:cpu,cpuacct:")) + member.toUtf8() + '\n'));
    const auto quota = [&](const QString &directory, const QString &value) {
        return unified ? writeThumbnailControlFile(directory + "/cpu.max", value.toUtf8() + " 100000\n")
            : writeThumbnailControlFile(directory + "/cpu.cfs_quota_us", value.toUtf8() + '\n')
                && writeThumbnailControlFile(directory + "/cpu.cfs_period_us", "100000\n");
    };
    if (member == "/") {
        QVERIFY(quota(mountpoint, leafQuota));
    } else {
        QVERIFY(quota(mountpoint, unified ? "max" : "-1"));
        QVERIFY(quota(mountpoint + "/team", parentQuota));
        QVERIFY(quota(mountpoint + "/team/leaf", leafQuota));
    }
    const QList<int> workers = workerCpus(fixture.path());
    QCOMPARE(workers.size(), qMin(CPU_COUNT(&affinity), expected));
    for (int cpu : workers) QVERIFY(CPU_ISSET(cpu, &affinity));
}

void TestStudio::refusesUnnameableThumbnailControlPaths_data()
{
    QTest::addColumn<QString>("field");
    for (const char *name : {"mountpoint", "mountroot", "member"})
        QTest::newRow(name) << QString::fromLatin1(name);
}

void TestStudio::refusesUnnameableThumbnailControlPaths()
{
    QFETCH(QString, field);
    QTemporaryDir fixture;
    QVERIFY(fixture.isValid());
    QByteArray root = "/", member = "/";
    QString mountpoint = fixture.filePath("cgroup");
    QByteArray rawMountpoint = QFile::encodeName(mountpoint);
    const QByteArray broken = QByteArray("broken-") + char(0xff);
    const QString replacement = QStringLiteral("broken-\ufffd");
    if (field == "mountpoint") {
        rawMountpoint += '/' + broken;
        mountpoint += '/' + replacement;
    } else if (field == "mountroot") {
        root += broken;
        member = root;
    } else {
        member += broken;
    }
    QVERIFY(writeThumbnailControlFile(fixture.filePath("self/mountinfo"),
        "32 1 0:30 " + root + ' ' + rawMountpoint + " rw - cgroup2 cgroup2 rw\n"));
    QVERIFY(writeThumbnailControlFile(fixture.filePath("self/cgroup"), "0::" + member + '\n'));
    QVERIFY(writeThumbnailControlFile(mountpoint + "/cpu.max", "max 100000\n"));
    if (field == "member")
        QVERIFY(writeThumbnailControlFile(mountpoint + '/' + replacement + "/cpu.max", "max 100000\n"));
    cpu_set_t allowed;
    QVERIFY(::sched_getaffinity(0, sizeof(allowed), &allowed) == 0);
    int first = 0;
    while (first < CPU_SETSIZE && !CPU_ISSET(first, &allowed)) ++first;
    QCOMPARE(workerCpus(fixture.path()), QList<int>{first});
}

void TestStudio::restrictsThumbnailWorkersToNativeAffinity()
{
    cpu_set_t original;
    QVERIFY(::sched_getaffinity(0, sizeof(original), &original) == 0);
    const auto restore = qScopeGuard([&] { ::sched_setaffinity(0, sizeof(original), &original); });
    QTemporaryDir fixture;
    QVERIFY(fixture.isValid());
    const QString mountpoint = fixture.filePath("cgroup");
    QVERIFY(writeThumbnailControlFile(fixture.filePath("self/mountinfo"),
        "32 1 0:30 / " + mountpoint.toUtf8() + " rw - cgroup2 cgroup2 rw\n"));
    QVERIFY(writeThumbnailControlFile(fixture.filePath("self/cgroup"), "0::/\n"));
    QVERIFY(writeThumbnailControlFile(mountpoint + "/cpu.max", "max 100000\n"));
    int cpu = 0;
    while (cpu < CPU_SETSIZE && !CPU_ISSET(cpu, &original)) ++cpu;
    QVERIFY(cpu < CPU_SETSIZE);
    cpu_set_t single;
    CPU_ZERO(&single);
    CPU_SET(cpu, &single);
    QVERIFY(::sched_setaffinity(0, sizeof(single), &single) == 0);
    QCOMPARE(workerCpus(fixture.path()), QList<int>{cpu});
    QCOMPARE(workerCpus("/nonexistent-thumbnail-proc"), QList<int>{cpu});
}

void TestStudio::assignsThumbnailHelpersToTheirSelectedCpu()
{
    cpu_set_t allowed;
    QVERIFY(::sched_getaffinity(0, sizeof(allowed), &allowed) == 0);
    QList<int> cpus;
    for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu)
        if (CPU_ISSET(cpu, &allowed)) cpus.append(cpu);
    QVERIFY(!cpus.isEmpty());
    for (int preferred : QList<int>{-1, cpus.first(), cpus.last(), CPU_SETSIZE}) {
        QProcess helper;
        const auto stop = qScopeGuard([&] {
            if (helper.state() != QProcess::NotRunning) {
                killHelper(helper);
                helper.waitForFinished(asyncTimeout);
            }
        });
        prepareHelper(helper, 0, 0, preferred);
        helper.start(QStringLiteral("/usr/bin/python3"),
                     {QStringLiteral("-c"), QStringLiteral("import os; print(','.join(map(str, sorted(os.sched_getaffinity(0)))))")});
        QTRY_VERIFY_WITH_TIMEOUT(helper.state() == QProcess::NotRunning, asyncTimeout);
        QCOMPARE(helper.exitStatus(), QProcess::NormalExit);
        QCOMPARE(helper.exitCode(), 0);
        const int selected = preferred >= 0 && preferred < CPU_SETSIZE
            && CPU_ISSET(preferred, &allowed) ? preferred : cpus.first();
        QCOMPARE(helper.readAllStandardOutput().trimmed(), QByteArray::number(selected));
    }
    cpu_set_t unchanged;
    QVERIFY(::sched_getaffinity(0, sizeof(unchanged), &unchanged) == 0);
    QVERIFY(CPU_EQUAL(&allowed, &unchanged));
}

static const char helperPolicyPython[] = R"PY(import os, resource, json
from pathlib import Path
report = json.dumps({
    'cpus': ','.join(map(str, sorted(os.sched_getaffinity(0)))),
    'cpu': list(resource.getrlimit(resource.RLIMIT_CPU)),
    'output': list(resource.getrlimit(resource.RLIMIT_FSIZE)),
    'core': list(resource.getrlimit(resource.RLIMIT_CORE)),
    'cgroup': Path('/proc/self/cgroup').read_text(),
})
)PY";

static QString nativeCpuList(const cpu_set_t &mask)
{
    QStringList cpus;
    for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu)
        if (CPU_ISSET(cpu, &mask)) cpus.append(QString::number(cpu));
    return cpus.join(',');
}

static QString nativeCgroupMembership()
{
    QFile file("/proc/self/cgroup");
    return file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.readAll()) : QString();
}

void TestStudio::preservesInheritedHelperAffinity_data()
{
    QTest::addColumn<bool>("singleCpu");
    QTest::newRow("one-allowed-cpu") << true;
    QTest::newRow("inherited-allowed-cpus") << false;
}

void TestStudio::preservesInheritedHelperAffinity()
{
    QFETCH(bool, singleCpu);
    cpu_set_t original;
    QVERIFY(::sched_getaffinity(0, sizeof(original), &original) == 0);
    const auto restore = qScopeGuard([&] { ::sched_setaffinity(0, sizeof(original), &original); });
    cpu_set_t expected = original;
    if (singleCpu) {
        int first = 0;
        while (first < CPU_SETSIZE && !CPU_ISSET(first, &original)) ++first;
        QVERIFY(first < CPU_SETSIZE);
        CPU_ZERO(&expected); CPU_SET(first, &expected);
        QVERIFY(::sched_setaffinity(0, sizeof(expected), &expected) == 0);
    } else if (CPU_COUNT(&original) < 2) {
        QSKIP("Multiple allowed CPUs are needed to distinguish inherited affinity from pinning");
    }
    const QString cgroup = nativeCgroupMembership();
    QVERIFY(!cgroup.isEmpty());
    QProcess helper;
    const auto stop = qScopeGuard([&] {
        if (helper.state() != QProcess::NotRunning) {
            killHelper(helper); helper.waitForFinished(asyncTimeout);
        }
    });
    QSignalSpy finished(&helper, &QProcess::finished);
    prepareHelper(helper, 5);
    helper.start("/usr/bin/python3", {"-c", QString::fromUtf8(helperPolicyPython) + "\nprint(report)\n"});
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, asyncTimeout);
    QCOMPARE(helper.exitStatus(), QProcess::NormalExit);
    QCOMPARE(helper.exitCode(), 0);
    const auto policy = QJsonDocument::fromJson(helper.readAllStandardOutput()).object();
    QCOMPARE(policy.value("cpus").toString(), nativeCpuList(expected));
    QCOMPARE(policy.value("cgroup").toString(), cgroup);
    QCOMPARE(policy.value("cpu").toArray(), (QJsonArray{5, 5}));
    QCOMPARE(policy.value("output").toArray(), (QJsonArray{double(helperOutputLimit), double(helperOutputLimit)}));
    QCOMPARE(policy.value("core").toArray(), (QJsonArray{0, 0}));
    cpu_set_t unchanged;
    QVERIFY(::sched_getaffinity(0, sizeof(unchanged), &unchanged) == 0);
    QVERIFY(CPU_EQUAL(&expected, &unchanged));
}

void TestStudio::inheritsPreviewHelperAffinity()
{
    cpu_set_t allowed;
    QVERIFY(::sched_getaffinity(0, sizeof(allowed), &allowed) == 0);
    if (CPU_COUNT(&allowed) < 2)
        QSKIP("Multiple allowed CPUs are needed to distinguish preview affinity from pinning");
    QTemporaryDir scratch;
    QVERIFY(scratch.isValid());
    QFile script(scratch.filePath("preview-policy"));
    QVERIFY(script.open(QIODevice::WriteOnly));
    const QByteArray program = QByteArray("#!/usr/bin/python3\n") + helperPolicyPython + R"PY(
import struct, sys
def string(value):
    data = value.encode('utf-16-be')
    return struct.pack('>I', len(data)) + data
packet = string('text') + string(report) + struct.pack('>BBiiI', 0, 0, 0, 0, 0xffffffff)
sys.stdout.buffer.write(struct.pack('>I', len(packet)) + packet)
sys.stdout.buffer.flush()
)PY";
    QCOMPARE(script.write(program), program.size()); script.close();
    QVERIFY(script.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
    const QString previous = setPreviewHelper(script.fileName());
    const auto restore = qScopeGuard([&] { setPreviewHelper(previous); });
    Preview preview;
    preview.setPath(m_tree.filePath("one.txt"));
    QTRY_COMPARE_WITH_TIMEOUT(preview.kind(), QStringLiteral("text"), asyncTimeout);
    QVERIFY(!preview.loading());
    const auto policy = QJsonDocument::fromJson(preview.text().toUtf8()).object();
    QCOMPARE(policy.value("cpus").toString(), nativeCpuList(allowed));
    QCOMPARE(policy.value("cgroup").toString(), nativeCgroupMembership());
    QCOMPARE(policy.value("core").toArray(), (QJsonArray{0, 0}));
    auto *process = preview.findChild<QProcess *>();
    QVERIFY(process);
    QTRY_COMPARE_WITH_TIMEOUT(process->state(), QProcess::NotRunning, asyncTimeout);
    cpu_set_t unchanged;
    QVERIFY(::sched_getaffinity(0, sizeof(unchanged), &unchanged) == 0);
    QVERIFY(CPU_EQUAL(&allowed, &unchanged));
}

void TestStudio::fillsThumbnailSlotsWithinCpuAndMemoryBudgets()
{
    ThumbnailFixture f;
    f.provider();
    const auto cpus = workerCpus();
    const int count = cpus.size();
    std::atomic_int entered = 0, active = 0, peak = 0;
    std::atomic_bool release = false;
    QMutex mutex;
    QSet<int> workerCpus;
    cpu_set_t nativeAffinity;
    QVERIFY(::sched_getaffinity(0, sizeof(nativeAffinity), &nativeAffinity) == 0);
    std::atomic_bool retainedAffinity = true;
    QList<qint64> budgets;
    const QString picture = f.source("result.png", Qt::green);
    const auto previous = setThumbnailRunner([&](const QStringList &args, const ThumbnailCancellation &cancel, qint64 budget, int cpu) {
        const int concurrent = ++active;
        int old = peak.load();
        while (old < concurrent && !peak.compare_exchange_weak(old, concurrent)) {}
        cpu_set_t affinity;
        if (::sched_getaffinity(0, sizeof(affinity), &affinity) == 0) {
            QMutexLocker lock(&mutex);
            if (!CPU_EQUAL(&affinity, &nativeAffinity)) retainedAffinity.store(false);
            workerCpus.insert(cpu);
            budgets.append(budget);
        }
        ++entered;
        while (!release.load() && !cancel->load()) QThread::msleep(1);
        const bool result = !cancel->load() && QFile::copy(picture, args.at(3));
        --active;
        return result;
    });
    const auto restore = qScopeGuard([&] {
        release.store(true);
        const bool finished = waitForThumbnailWorkers();
        setThumbnailRunner(previous);
        QVERIFY(finished);
    });
    std::vector<std::unique_ptr<Thumbnail>> items;
    for (int i = 0; i < count + 2; ++i) {
        auto item = std::make_unique<Thumbnail>();
        item->setPath(f.pdf(QStringLiteral("parallel-%1.pdf").arg(i)));
        item->setRequested(true);
        items.push_back(std::move(item));
    }
    QTRY_COMPARE_WITH_TIMEOUT(entered.load(), count, asyncTimeout);
    QCOMPARE(active.load(), count);
    QCOMPARE(peak.load(), count);
    {
        QMutexLocker lock(&mutex);
        QCOMPARE(budgets.size(), count);
        for (qint64 budget : budgets) QCOMPARE(budget, (512LL * 1024 * 1024) / count);
        QVERIFY(retainedAffinity.load());
        QCOMPARE(workerCpus, count > 1 ? QSet<int>(cpus.begin(), cpus.end()) : QSet<int>{-1});
    }
    QCoreApplication::processEvents(QEventLoop::AllEvents);
    const QString cancelled = items[count]->path();
    items[count]->setRequested(false);
    items[0]->setRequested(false);
    QTRY_COMPARE_WITH_TIMEOUT(entered.load(), count + 1, asyncTimeout);
    QCOMPARE(peak.load(), count);
    QTimer heartbeat;
    int ticks = 0;
    connect(&heartbeat, &QTimer::timeout, &heartbeat, [&] { ++ticks; });
    heartbeat.start(5);
    QTRY_VERIFY(ticks > 0);
    release.store(true);
    for (int i = 1; i < count + 2; ++i)
        if (i != count) QTRY_VERIFY(items[i]->ready());
    QTRY_COMPARE(active.load(), 0);
    QVERIFY(!items[0]->ready());
    QVERIFY(!items[count]->ready());
    QVERIFY(!QFileInfo::exists(f.cached(cancelled)));
    QCOMPARE(entered.load(), count + 1);
}

void TestStudio::cancelsThumbnailWorkAndKeepsTheGuiResponsive()
{
    ThumbnailFixture f;
    f.provider();
    std::atomic_int calls = 0;
    std::atomic_bool worker = false;
    const int workers = workerCpus().size();
    const auto finished = std::make_shared<std::atomic_int>(0);
    const auto previous = setThumbnailRunner([&, finished](const QStringList &args, const ThumbnailCancellation &cancel, qint64, int) {
        if (args.contains(f.root.filePath("queued.pdf"))) { ++calls; return false; }
        if (!args.at(1).contains("slow-")) return false;
        worker.store(QThread::currentThread() != qApp->thread()); ++calls;
        while (!cancel->load()) QThread::msleep(1);
        ++*finished;
        return false;
    });
    const auto restore = qScopeGuard([&] {
        const bool complete = waitForThumbnailWorkers();
        setThumbnailRunner(previous);
        QVERIFY(complete);
    });
    std::vector<std::unique_ptr<Thumbnail>> active;
    for (int i = 0; i < workers; ++i) {
        auto next = std::make_unique<Thumbnail>();
        next->setPath(f.pdf(QStringLiteral("slow-%1.pdf").arg(i)));
        next->setRequested(true);
        active.push_back(std::move(next));
    }
    auto &item = *active.front();
    QTRY_COMPARE(calls.load(), workers);
    QVERIFY(worker.load());
    QTimer heartbeat; int ticks = 0;
    connect(&heartbeat, &QTimer::timeout, &heartbeat, [&] { ++ticks; });
    heartbeat.start(5);
    QTRY_VERIFY(ticks >= 3);
    Thumbnail queued;
    queued.setPath(f.pdf("queued.pdf")); queued.setRequested(true);
    QCoreApplication::processEvents(QEventLoop::AllEvents);
    queued.setRequested(false);
    const QString cancelledPath = item.path();
    for (auto &next : active) next->setRequested(false);
    QTRY_COMPARE(finished->load(), workers);
    QCOMPARE(calls.load(), workers);
    QVERIFY(!item.ready());
    const QString next = f.source("new.png", Qt::blue);
    QVERIFY(!next.isEmpty());
    item.setPath(next); item.setRequested(true);
    QTRY_VERIFY(item.ready());
    QCOMPARE(calls.load(), workers);
    QVERIFY(!QFileInfo::exists(f.cached(cancelledPath)));
    QVERIFY(!QFileInfo::exists(f.cached(cancelledPath, "fail/omafiles-" + QStringLiteral(OMAFILES_VERSION))));
    item.setRequested(false);
    QVERIFY(item.ready());
    item.setPath(f.root.filePath("missing.png"));
    QVERIFY(!item.ready());
}

void TestStudio::thumbnailsOnlyVisibleRowsInEveryFileView()
{
    const QString folder = m_tree.filePath("pictures");
    QVERIFY(QDir().mkpath(folder));
    const auto remove = qScopeGuard([&] { tab()->setCwd(m_tree.path()); settle(); QDir(folder).removeRecursively(); });
    QImage image(256, 128, QImage::Format_ARGB32); image.fill(Qt::magenta);
    for (int i = 0; i < 120; ++i)
        QVERIFY(image.save(QDir(folder).filePath(QStringLiteral("image-%1.png").arg(i, 3, 10, QLatin1Char('0')))));
    tab()->setCwd(folder); settle();
    for (const QString &mode : {QStringLiteral("list"), QStringLiteral("details"), QStringLiteral("icons")}) {
        tab()->setViewMode(mode); settle();
        auto *view = filesRoot()->property("view").value<QObject *>();
        QVERIFY(view);
        const int buffer = view->property("cacheBuffer").toInt();
        view->setProperty("cacheBuffer", 1000);
        const auto restoreBuffer = qScopeGuard([&] { view->setProperty("cacheBuffer", buffer); });
        const auto previews = [&] {
            QList<Thumbnail *> result;
            for (auto *item : itemsNamed("thumbnail"))
                if (auto *preview = qobject_cast<Thumbnail *>(item)) result.append(preview);
            return result;
        };
        const auto hasReady = [&] {
            const auto items = previews();
            return std::any_of(items.cbegin(), items.cend(), [](Thumbnail *item) { return item->ready(); });
        };
        const auto hasLast = [&] {
            const auto items = previews();
            return std::any_of(items.cbegin(), items.cend(), [](Thumbnail *item) {
                return item->path().endsWith("image-119.png") && item->ready();
            });
        };
        QTRY_VERIFY(!previews().isEmpty());
        QTRY_VERIFY(hasReady());
        for (auto *item : previews()) {
            if (!item->requested()) continue;
            const QPointF pos = item->mapToScene(QPointF());
            QVERIFY(pos.y() + item->height() > 40);
            QVERIFY(pos.y() < window()->height() - 26);
        }
        key(Qt::Key_End);
        QTRY_VERIFY(hasLast());
        key(Qt::Key_Home);
        const int columns = mode == "icons"
            ? qMax(1, int(view->property("width").toReal()) / 112) : 1;
        const int height = mode == "icons" ? 92 : 30;
        const int capacity = (int(view->property("height").toReal()) / height + 2) * columns;
        const auto requestedCount = [&] {
            const auto items = previews();
            return std::count_if(items.cbegin(), items.cend(), [](Thumbnail *item) { return item->requested(); });
        };
        QTRY_VERIFY(requestedCount() <= capacity);
        focusRow("image-000.png");
        QTRY_VERIFY(requestedCount() > 0);
        // The preview must keep the thumbnails already shown, so one is shown first.
        QTRY_VERIFY(hasReady());
        key('i');
        auto *sheet = window()->findChild<QQuickItem *>("previewSheet");
        QVERIFY(sheet);
        QTRY_VERIFY(sheet->isVisible());
        QTRY_COMPARE(requestedCount(), 0);
        QVERIFY(hasReady());
        key(Qt::Key_Escape);
        QTRY_VERIFY(!sheet->isVisible());
        QTRY_VERIFY(requestedCount() > 0);
        QTRY_VERIFY(hasReady());
    }
}


void TestStudio::restoresInterruptedOverlays_data()
{
    QTest::addColumn<QString>("name");
    for (const char *name : {"helpSheet", "tagSheet", "modeSheet", "commandPalette",
                            "fileMenu", "tabMenu", "openWithMenu", "scriptsMenu",
                            "viewMenu", "filterMenu", "columnMenu", "newTabMenu"})
        QTest::newRow(name) << QString::fromLatin1(name);
}

void TestStudio::restoresInterruptedOverlays()
{
    QFETCH(QString, name);
    focusRow("three.ods");
    auto *overlay = window()->findChild<QQuickItem *>(name);
    QVERIFY(overlay);
    if (name == "tagSheet") {
        QVERIFY(QMetaObject::invokeMethod(overlay, "openFor",
            Q_ARG(QVariant, QStringList{m_tree.filePath("three.ods")}),
            Q_ARG(QVariant, "add"), Q_ARG(QVariant, "keep this text")));
    } else if (name == "modeSheet") {
        QVERIFY(QMetaObject::invokeMethod(overlay, "openFor",
            Q_ARG(QVariant, QStringList{m_tree.filePath("three.ods")}),
            Q_ARG(QVariant, (QVariantMap{{"permissions", 0644}, {"mine", true}}))));
    } else if (name.endsWith("Menu")) {
        QVERIFY(QMetaObject::invokeMethod(overlay, "openAt",
            Q_ARG(QVariant, 200), Q_ARG(QVariant, 200)));
    } else {
        QVERIFY(QMetaObject::invokeMethod(overlay, "open"));
    }
    settle();
    if (name == "tagSheet" || name == "modeSheet")
        key(Qt::Key_Tab);
    QPointer<QQuickItem> interrupted = window()->activeFocusItem();
    QVERIFY(interrupted);
    QVERIFY(overlay->isAncestorOf(interrupted));
    FileManager manager;
    manager.setWorkspace(m_workspace);
    const QString uri = QUrl::fromLocalFile(m_tree.filePath("three.ods")).toString();
    for (int repeat = 0; repeat < 2; ++repeat) {
        manager.ShowItemProperties({uri}, "");
        settle();
        QPointer<QObject> dialog = window()->findChild<QObject *>("propertiesDialog");
        QVERIFY(dialog && dialog->property("visible").toBool());
        QVERIFY(window()->activeFocusItem() != interrupted);
        QVERIFY(QMetaObject::invokeMethod(window(), "takeFocus"));
        QVERIFY(window()->activeFocusItem() != interrupted);
        if (repeat == 0)
            key(Qt::Key_Escape);
        else
            QVERIFY(QMetaObject::invokeMethod(dialog, "close"));
        QTRY_COMPARE(window()->activeFocusItem(), interrupted.data());
        QVERIFY(overlay->isVisible());
    }
    if (name == "tagSheet")
        QCOMPARE(overlay->findChild<QObject *>("tagSource")->property("text").toString(),
                 QStringLiteral("keep this text"));
    key(Qt::Key_Escape);
    QTRY_VERIFY(!overlay->isVisible());
    QVERIFY(!window()->property("blocked").toBool());
    const int before = cursor();
    key('j');
    QVERIFY(cursor() != before);
}

void TestStudio::restoresTheTopOverlayWhenOthersClose()
{
    auto *help = itemsNamed("helpSheet").first();
    auto *palette = itemsNamed("commandPalette").first();
    auto *menu = itemsNamed("fileMenu").first();
    QVERIFY(QMetaObject::invokeMethod(help, "open"));
    QVERIFY(QMetaObject::invokeMethod(palette, "open"));
    settle();
    QPointer<QQuickItem> query = window()->activeFocusItem();
    QVERIFY(palette->isAncestorOf(query));
    QVERIFY(palette->z() > help->z());
    QVERIFY(QMetaObject::invokeMethod(menu, "openAt", Q_ARG(QVariant, 200), Q_ARG(QVariant, 200)));
    settle();
    QVERIFY(menu->z() > palette->z());
    QVERIFY(QMetaObject::invokeMethod(help, "close"));
    settle();
    QVERIFY(menu->isAncestorOf(window()->activeFocusItem()));
    key(Qt::Key_Escape);
    QTRY_COMPARE(window()->activeFocusItem(), query.data());
    key(Qt::Key_Escape);
    QTRY_VERIFY(!window()->property("blocked").toBool());

    QVERIFY(QMetaObject::invokeMethod(help, "open"));
    m_workspace->propertiesRequested("An asynchronous receipt");
    settle();
    QVERIFY(QMetaObject::invokeMethod(help, "close"));
    auto *text = window()->findChild<QQuickItem *>("propertyText");
    QTRY_COMPARE(window()->activeFocusItem(), text);
    key(Qt::Key_Escape);
    QTRY_VERIFY(!window()->property("blocked").toBool());
    const int before = cursor();
    key('j');
    QVERIFY(cursor() != before);
}

void TestStudio::stacksAsynchronousReceipts()
{
    auto *help = itemsNamed("helpSheet").first();
    QVERIFY(QMetaObject::invokeMethod(help, "open"));
    settle();
    QPointer<QQuickItem> underlying = window()->activeFocusItem();
    m_index->defaultManagerFinished(true, "Installed successfully. Undo: saved receipt");
    settle();
    QPointer<QQuickItem> receipt = window()->activeFocusItem();
    QVERIFY(receipt);
    QCOMPARE(receipt->property("text").toString(), QStringLiteral("Installed successfully. Undo: saved receipt"));
    key('a', Qt::ControlModifier);
    const int selectionEnd = receipt->property("selectionEnd").toInt();
    QVERIFY(selectionEnd > 0);
    for (int repeat = 0; repeat < 2; ++repeat) {
        m_workspace->propertiesRequested("Properties of another file");
        settle();
        QVERIFY(window()->activeFocusItem() != receipt);
        QVERIFY(receipt->isVisible());
        if (repeat == 0) {
            key(Qt::Key_Escape);
        } else {
            QObject *top = nullptr;
            for (auto *dialog : window()->findChildren<QObject *>("propertiesDialog"))
                if (dialog->property("visible").toBool()
                    && dialog->property("receipt").toString() == "Properties of another file")
                    top = dialog;
            QVERIFY(top);
            QVERIFY(QMetaObject::invokeMethod(top, "close"));
        }
        QTRY_COMPARE(window()->activeFocusItem(), receipt.data());
        QCOMPARE(receipt->property("selectionEnd").toInt(), selectionEnd);
    }
    key(Qt::Key_Escape);
    QTRY_COMPARE(window()->activeFocusItem(), underlying.data());
    key(Qt::Key_Escape);
    QTRY_VERIFY(!window()->property("blocked").toBool());
}

void TestStudio::preservesFocusInCommandsOpenedByOverlays()
{
    focusRow("three.ods");
    key(':');
    for (char ch : QByteArray("rename")) key(ch);
    key(Qt::Key_Return);
    QTRY_VERIFY(!window()->property("prompt").toString().isEmpty());
    QVERIFY(window()->activeFocusItem());
    QVERIFY(itemsNamed("filterEntry").first()->isAncestorOf(window()->activeFocusItem()));
    key(Qt::Key_Escape);
    key(':');
    for (char ch : QByteArray("show keys")) key(ch);
    key(Qt::Key_Return);
    auto *help = itemsNamed("helpSheet").first();
    QTRY_VERIFY(help->isVisible());
    QVERIFY(help->isAncestorOf(window()->activeFocusItem()));
    key(Qt::Key_Escape);
    QTRY_VERIFY(!window()->property("blocked").toBool());
}


void TestStudio::restoresAvailableOverlayFocusTargets()
{
    auto *help = itemsNamed("helpSheet").first();
    QVERIFY(QMetaObject::invokeMethod(help, "open"));
    auto *temporary = new QQuickItem(help);
    temporary->forceActiveFocus();
    QCOMPARE(window()->activeFocusItem(), temporary);
    m_workspace->propertiesRequested("Receipt");
    settle();
    temporary->forceActiveFocus();
    auto *receipt = window()->findChild<QQuickItem *>("propertyText");
    QTRY_COMPARE(window()->activeFocusItem(), receipt);
    delete temporary;
    key(Qt::Key_Escape);
    QTRY_VERIFY(help->isAncestorOf(window()->activeFocusItem()));
    key(Qt::Key_Escape);
    QTRY_VERIFY(!window()->property("blocked").toBool());

    auto *origin = new QQuickItem(window()->contentItem());
    origin->forceActiveFocus();
    QVERIFY(QMetaObject::invokeMethod(help, "open"));
    settle();
    key(Qt::Key_Escape);
    QTRY_COMPARE(window()->activeFocusItem(), origin);
    m_workspace->propertiesRequested("Receipt");
    settle();
    delete origin;
    key(Qt::Key_Escape);
    const int before = cursor();
    key('j');
    QVERIFY(cursor() != before);
}


void TestStudio::preservesInterruptedTextSelections_data()
{
    QTest::addColumn<char>("opener");
    QTest::newRow("rename") << 'r';
    QTest::newRow("new-folder") << 'n';
    QTest::newRow("palette") << ':';
    QTest::newRow("tag-editor") << 't';
}

void TestStudio::preservesInterruptedTextSelections()
{
    QFETCH(char, opener);
    focusRow("three.ods");
    key(opener);
    if (opener == 't') key('a');
    auto *input = window()->activeFocusItem();
    QVERIFY(input);
    key('a', Qt::ControlModifier);
    for (char ch : QByteArray("abcdef")) key(ch);
    key(Qt::Key_Left);
    key(Qt::Key_Left, Qt::ShiftModifier);
    QCOMPARE(input->property("selectionStart").toInt(), 4);
    QCOMPARE(input->property("selectionEnd").toInt(), 5);
    FileManager manager;
    manager.setWorkspace(m_workspace);
    manager.ShowItemProperties({QUrl::fromLocalFile(m_tree.filePath("one.txt")).toString()}, "");
    settle();
    key(Qt::Key_Escape);
    QTRY_COMPARE(window()->activeFocusItem(), input);
    QCOMPARE(input->property("selectionStart").toInt(), 4);
    QCOMPARE(input->property("selectionEnd").toInt(), 5);
    key('X');
    QCOMPARE(input->property("text").toString(), QStringLiteral("abcdXf"));
    key(Qt::Key_Escape);
}

void TestStudio::boundsThumbnailHelpersWithoutCachingExecutionFailures()
{
    ThumbnailFixture f;
    const auto previous = setThumbnailRunner({});
    const auto restore = qScopeGuard([&] { setThumbnailRunner(previous); });
    const QString picture = f.source("output.png", Qt::yellow);
    const QString helper = f.root.filePath("helper.py");
    QFile script(helper);
    QVERIFY(script.open(QIODevice::WriteOnly));
    script.write(R"PY(import sys, os, mmap, resource, shutil, time, subprocess
mode, source, output, picture = sys.argv[1:]
assert len(os.sched_getaffinity(0)) == 1
assert resource.getrlimit(resource.RLIMIT_AS)[0] == resource.RLIM_INFINITY
assert resource.getrlimit(resource.RLIMIT_CPU)[0] == 5
assert resource.getrlimit(resource.RLIMIT_FSIZE)[0] == 8 * 1024 * 1024
reserved = mmap.mmap(-1, 1024 * 1024 * 1024)
if mode == 'exit': sys.exit(75)
if mode == 'signal': os.kill(os.getpid(), 9)
if mode == 'timeout': time.sleep(8)
if mode == 'memory':
    subprocess.run(['/usr/bin/python3', '-c', 'import time; data=bytearray(544*1024*1024); time.sleep(0.5)'], check=True)
if mode == 'output':
    with open(output, 'wb') as f: f.write(b'x' * (9*1024*1024))
shutil.copyfile(picture, output)
)PY");
    script.close();
    QVERIFY(QDir().mkpath(f.root.filePath("data/thumbnailers")));
    const QString path = f.pdf("resource.pdf");
    const auto descriptor = [&](const QString &mode) {
        QFile file(f.root.filePath("data/thumbnailers/helper.thumbnailer"));
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
        return file.write(QString("[Thumbnailer Entry]\nExec=/usr/bin/python3 %1 %2 %i %o %3\nMimeType=application/pdf;\n")
            .arg(helper, mode, picture).toUtf8()) > 0;
    };
    for (const QString &mode : {QStringLiteral("exit"), QStringLiteral("signal"),
            QStringLiteral("timeout"), QStringLiteral("memory"), QStringLiteral("output")}) {
        QVERIFY(descriptor(mode));
        QVERIFY(thumbnailFor(path, 128, f.token()).isNull());
        QVERIFY(!QFileInfo::exists(f.cached(path, "fail/omafiles-" + QStringLiteral(OMAFILES_VERSION))));
    }
    QVERIFY(descriptor("ok"));
    const QString fail = f.cached(path, "fail/omafiles-" + QStringLiteral(OMAFILES_VERSION));
    QVERIFY(QDir().mkpath(typedDirectory(fail, nullptr)));
    QImage unclassified(1, 1, QImage::Format_ARGB32); unclassified.fill(Qt::transparent);
    unclassified.setText("Thumb::URI", QString::fromUtf8(QUrl::fromLocalFile(path).toEncoded()));
    unclassified.setText("Thumb::MTime", QString::number(QFileInfo(path).lastModified().toSecsSinceEpoch()));
    unclassified.setText("Thumb::Size", QString::number(QFileInfo(path).size()));
    QVERIFY(unclassified.save(fail));
    const QImage image = thumbnailFor(path, 128, f.token());
    QVERIFY(!image.isNull());
    QCOMPARE(image.pixelColor(20, 20), QColor(Qt::yellow));
}

void TestStudio::refreshesSameVersionThumbnailSources()
{
    ThumbnailFixture f;
    const QString path = f.source("same.png");
    Thumbnail item;
    item.setWidth(128); item.setHeight(128);
    item.setPath(path); item.setRequested(true);
    QTRY_VERIFY(item.ready());
    {
        Thumbnail second;
        second.setPath(path); second.setRequested(true);
        QTRY_VERIFY(second.ready());
    }
    struct stat before {};
    QVERIFY(::stat(QFile::encodeName(path).constData(), &before) == 0);
    const auto color = [&] {
        QImage frame(128, 128, QImage::Format_ARGB32); frame.fill(Qt::transparent);
        QPainter painter(&frame); item.paint(&painter); painter.end();
        return frame.pixelColor(64, 64);
    };
    QCOMPARE(color(), QColor(Qt::red));
    QVERIFY(!f.source("same.png", Qt::blue).isEmpty());
    const timespec times[] = {before.st_atim, before.st_mtim};
    QVERIFY(::utimensat(AT_FDCWD, QFile::encodeName(path).constData(), times, 0) == 0);
    QTRY_COMPARE(color(), QColor(Qt::blue));
    QVERIFY(QFile::remove(path));
    QTRY_VERIFY(!item.ready());
    QVERIFY(!f.source("same.png", Qt::green).isEmpty());
    QTRY_COMPARE(color(), QColor(Qt::green));
}

void TestStudio::refreshesVisibleThumbnails_data()
{
    QTest::addColumn<QString>("mode");
    QTest::addColumn<bool>("tagged");
    QTest::newRow("list") << QStringLiteral("list") << false;
    QTest::newRow("details") << QStringLiteral("details") << false;
    QTest::newRow("icons") << QStringLiteral("icons") << false;
    QTest::newRow("tag-list") << QStringLiteral("list") << true;
}

void TestStudio::refreshesVisibleThumbnails()
{
    QFETCH(QString, mode);
    QFETCH(bool, tagged);
    ThumbnailFixture f;
    QVERIFY(QDir().mkpath(f.root.filePath("pictures")));
    const QString path = f.source("pictures/visible.png");
    const QString broken = f.root.filePath("pictures/repair.png");
    { QFile file(broken); QVERIFY(file.open(QIODevice::WriteOnly)); file.write("broken image"); }
    const auto cleanup = qScopeGuard([&] { tab()->setSelectedTag({}); tab()->setCwd(m_tree.path()); writeIndex(); m_index->reload(); settle(); });
    tab()->setCwd(f.root.filePath("pictures"));
    if (tagged) {
        putIndexRow(path, {"live-thumbnail"}); putIndexRow(broken, {"live-thumbnail"});
        m_index->reload(); tab()->setSelectedTag("live-thumbnail");
    }
    tab()->setViewMode(mode); settle();
    focusRow("visible.png");
    const auto preview = [&](const QString &source) -> Thumbnail * {
        for (auto *item : itemsNamed("thumbnail"))
            if (auto *thumb = qobject_cast<Thumbnail *>(item); thumb && thumb->path() == source)
                return thumb;
        return nullptr;
    };
    const auto painted = [&](const QString &source) {
        auto *item = preview(source);
        if (!item || !item->ready()) return QColor();
        QImage frame(128, 128, QImage::Format_ARGB32); frame.fill(Qt::transparent);
        QPainter painter(&frame); item->paint(&painter); painter.end();
        return frame.pixelColor(int(item->width()/2), int(item->height()/2));
    };
    QTRY_VERIFY(preview(path) && preview(path)->ready());
    QCOMPARE(painted(path), QColor(Qt::red));
    const qint64 oldSize = filesRoot()->property("entry").toMap().value("size").toLongLong();
    QImage image(90, 60, QImage::Format_ARGB32); image.fill(Qt::cyan);
    QVERIFY(image.save(path));
    QVERIFY(QFileInfo(path).size() != oldSize);
    QTRY_COMPARE(painted(path), QColor(Qt::cyan));
    QTRY_COMPARE(filesRoot()->property("entry").toMap().value("size").toLongLong(), QFileInfo(path).size());
    QTRY_VERIFY(preview(broken));
    auto *unreadable = preview(broken);
    unreadable->setRequested(false);
    QSignalSpy decoded(unreadable, &Thumbnail::readyChanged);
    unreadable->setRequested(true);
    QTRY_VERIFY(!decoded.isEmpty());
    QVERIFY(!unreadable->ready());
    image.fill(Qt::magenta); QVERIFY(image.save(broken));
    QTRY_COMPARE(painted(broken), QColor(Qt::magenta));
    QCOMPARE(tab()->viewMode(), mode);
    QCOMPARE(tab()->cursorPath(), path);
}

void TestStudio::doesNotCacheThumbnailAdmissionLimits()
{
    ThumbnailFixture f;
    const QString path = f.root.filePath("large.png");
    QImage image(8193, 8193, QImage::Format_Grayscale8); image.fill(128);
    QVERIFY(image.save(path));
    image = {};
    QVERIFY(thumbnailFor(path, 128, f.token()).isNull());
    QVERIFY(!QFileInfo::exists(f.cached(path, "fail/omafiles-" + QStringLiteral(OMAFILES_VERSION))));
}

void TestStudio::containsInlinePromptFocus_data()
{
    QTest::addColumn<QString>("mode");
    QTest::addColumn<QSize>("size");
    QTest::addColumn<bool>("interrupted");
    for (const char *mode : {"rename", "folder", "match", "find", "view", "set", "filter", "path", "palette-folder"})
        for (const QSize size : {QSize(1200, 800), QSize(400, 300)})
            for (bool interrupted : {false, true})
                QTest::newRow(qPrintable(QStringLiteral("%1-%2-%3").arg(mode).arg(size.width()).arg(interrupted)))
                    << QString::fromLatin1(mode) << size << interrupted;
}

void TestStudio::containsInlinePromptFocus()
{
    QFETCH(QString, mode);
    QFETCH(QSize, size);
    QFETCH(bool, interrupted);
    const QSize original = window()->size();
    auto *folder = qobject_cast<FolderModel *>(filesRoot()->property("folder").value<QObject *>());
    QVERIFY(folder);
    folder->clearMarks();
    const auto restore = qScopeGuard([&] { folder->clearMarks(); window()->resize(original); });
    window()->resize(size);
    focusRow("one.txt");
    openInlinePrompt(mode);
    settle();
    QPointer<QQuickItem> input = window()->activeFocusItem();
    QVERIFY(input && input->inherits("QQuickTextInput"));
    key('a', Qt::ControlModifier);
    for (char ch : QByteArray("keep draft")) key(ch);
    key(Qt::Key_Left); key(Qt::Key_Left, Qt::ShiftModifier);
    const int selectionStart = input->property("selectionStart").toInt();
    const int selectionEnd = input->property("selectionEnd").toInt();
    if (interrupted) {
        FileManager manager;
        manager.setWorkspace(m_workspace);
        manager.ShowItemProperties({QUrl::fromLocalFile(m_tree.filePath("one.txt")).toString()}, "");
        settle();
        QVERIFY(window()->activeFocusItem() != input);
        key(Qt::Key_Escape);
        QTRY_COMPARE(window()->activeFocusItem(), input.data());
    }
    for (int repeat = 0; repeat < 3; ++repeat) {
        auto *status = itemsNamed("statusBar").first();
        QTRY_VERIFY(status->property("firstHint").value<QQuickItem *>() != input.data());
        QTRY_VERIFY(status->property("lastHint").value<QQuickItem *>() != input.data());
        auto *first = status->property("firstHint").value<QQuickItem *>();
        auto *last = status->property("lastHint").value<QQuickItem *>();
        key(Qt::Key_Tab);
        QTRY_COMPARE(window()->activeFocusItem(), first ? first : input.data());
        key(Qt::Key_Backtab, Qt::ShiftModifier);
        QTRY_COMPARE(window()->activeFocusItem(), input.data());
        key(Qt::Key_Backtab, Qt::ShiftModifier);
        QTRY_COMPARE(window()->activeFocusItem(), last ? last : input.data());
        key(Qt::Key_Tab);
        QTRY_COMPARE(window()->activeFocusItem(), input.data());
    }
    window()->contentItem()->forceActiveFocus();
    QTRY_COMPARE(window()->activeFocusItem(), input.data());
    QVERIFY(QMetaObject::invokeMethod(window(), "takeFocus"));
    QTRY_COMPARE(window()->activeFocusItem(), input.data());
    QCOMPARE(input->property("text").toString(), QStringLiteral("keep draft"));
    QCOMPARE(input->property("selectionStart").toInt(), selectionStart);
    QCOMPARE(input->property("selectionEnd").toInt(), selectionEnd);
    QTRY_VERIFY(QRectF(QPointF(), window()->size()).contains(input->mapToScene(QPointF(input->width()/2, input->height()/2))));
    key(Qt::Key_Tab);
    key(Qt::Key_Escape);
    QTRY_VERIFY(!window()->property("blocked").toBool());
    key(Qt::Key_Home); key('j');
    QCOMPARE(cursor(), 1);
    key('?');
    QTRY_VERIFY(itemsNamed("helpSheet").first()->isVisible());
    key(Qt::Key_Escape);
    QTRY_VERIFY(!window()->property("blocked").toBool());
}

void TestStudio::returnsInlinePromptsToTheList()
{
    focusRow("one.txt");
    auto *listFocus = window()->activeFocusItem();
    QQuickItem *toolbar = nullptr;
    for (auto *item : window()->findChildren<QQuickItem *>())
        if (item->property("caption").toString() == "Reload index") { toolbar = item; break; }
    QVERIFY(toolbar);
    toolbar->forceActiveFocus();
    QCOMPARE(window()->activeFocusItem(), toolbar);
    QVERIFY(QMetaObject::invokeMethod(window(), "startMakingFolder"));
    settle();
    QVERIFY(window()->activeFocusItem()->inherits("QQuickTextInput"));
    key(Qt::Key_Escape);
    QTRY_COMPARE(window()->activeFocusItem(), listFocus);
    key('j');
    QVERIFY(cursor() != rowOf("one.txt"));
}

void TestStudio::restoresInlinePromptFocusTargets()
{
    for (bool path : {false, true}) {
        window()->setProperty(path ? "editingPath" : "prompt", path ? QVariant(true) : QVariant("folder"));
        QVERIFY(QMetaObject::invokeMethod(window(), "takeFocus"));
        QTRY_VERIFY(window()->activeFocusItem() && window()->activeFocusItem()->inherits("QQuickTextInput"));
        key(Qt::Key_Escape);
        QTRY_VERIFY(!window()->property("blocked").toBool());
    }
}

static QByteArray silentWave()
{
    QByteArray wave;
    QDataStream audio(&wave, QIODevice::WriteOnly);
    audio.setByteOrder(QDataStream::LittleEndian);
    audio.writeRawData("RIFF", 4); audio << quint32(36 + 352800);
    audio.writeRawData("WAVEfmt ", 8); audio << quint32(16) << quint16(1) << quint16(1)
        << quint32(44100) << quint32(88200) << quint16(2) << quint16(16);
    audio.writeRawData("data", 4); audio << quint32(352800);
    wave += QByteArray(352800, '\0');
    return wave;
}

class ScriptedPlayback
{
public:
    explicit ScriptedPlayback(int acknowledgementDelay = 0) {
        QFile script(root.filePath("playback"));
        if (!script.open(QIODevice::WriteOnly)) return;
        script.write(QByteArray(R"PY(#!/usr/bin/python3
import struct, sys, time
playing, position = False, 0
def string(value):
    data = value.encode('utf-16-be')
    return struct.pack('>I', len(data)) + data
def send():
    caption = f"{'Playing' if playing else 'Paused'} · 0:{position:02d} / 0:04"
    packet = string('audio') + string(caption) + struct.pack('>BBiiI', playing, False, 0, 0, 0xffffffff)
    sys.stdout.buffer.write(struct.pack('>I', len(packet)) + packet)
    sys.stdout.buffer.flush()
send()
for line in sys.stdin:
    command = line.strip()
    if command == 'toggle':
        time.sleep(ACKNOWLEDGEMENT_DELAY / 1000)
        playing = not playing
    elif command == 'pause': playing = False
    elif command.startswith('seek '): position = max(0, min(4, position + int(command[5:])))
    send()
)PY").replace("ACKNOWLEDGEMENT_DELAY", QByteArray::number(acknowledgementDelay)));
        script.close();
        if (!script.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner)) return;
        previous = setPreviewHelper(script.fileName());
        valid = true;
    }
    ~ScriptedPlayback() { if (valid) setPreviewHelper(previous); }
    bool isValid() const { return valid; }
private:
    QTemporaryDir root;
    QString previous;
    bool valid = false;
};

void TestStudio::waitsForDelayedPlaybackAcknowledgement()
{
    ScriptedPlayback helper(6000);
    QVERIFY(helper.isValid());
    Preview preview;
    preview.setPath(m_tree.filePath("one.txt"));
    QTRY_COMPARE(preview.kind(), QStringLiteral("audio"));
    bool acknowledged = false;
    connect(&preview, &Preview::changed, &preview, [&] { acknowledged |= preview.playing(); });
    preview.togglePlayback();
    QTRY_VERIFY2(acknowledged, qPrintable(preview.text()));
    QVERIFY(preview.playing());
    preview.pause();
    QTRY_VERIFY(!preview.playing());
}

void TestStudio::boundsPreviewImageTransfers_data()
{
    QTest::addColumn<QByteArray>("format");
    QTest::addColumn<QSize>("original");
    QTest::addColumn<QSize>("expected");
    QTest::addColumn<QString>("suffix");
    QTest::addColumn<bool>("rotated");
    QTest::newRow("single-pixel") << QByteArray("PNG") << QSize(1, 1) << QSize(1, 1) << QString("png") << false;
    QTest::newRow("native-png") << QByteArray("PNG") << QSize(256, 256) << QSize(256, 256) << QString("png") << false;
    QTest::newRow("native-misnamed-png") << QByteArray("PNG") << QSize(256, 256) << QSize(256, 256) << QString("jpg") << false;
    QTest::newRow("landscape-limit") << QByteArray("PNG") << QSize(1280, 800) << QSize(1280, 800) << QString("png") << false;
    QTest::newRow("portrait-limit") << QByteArray("PNG") << QSize(800, 1280) << QSize(800, 1280) << QString("png") << false;
    QTest::newRow("large-landscape") << QByteArray("PNG") << QSize(2400, 1200) << QSize(1280, 640) << QString("png") << false;
    QTest::newRow("large-portrait") << QByteArray("PNG") << QSize(1200, 2400) << QSize(640, 1280) << QString("png") << false;
    QTest::newRow("large-square") << QByteArray("PNG") << QSize(2000, 2000) << QSize(1280, 1280) << QString("png") << false;
    QTest::newRow("wide-image") << QByteArray("PNG") << QSize(4096, 64) << QSize(1280, 20) << QString("png") << false;
    QTest::newRow("misnamed-bmp") << QByteArray("BMP") << QSize(23, 17) << QSize(23, 17) << QString("bin") << false;
    QTest::newRow("misnamed-jpeg") << QByteArray("JPEG") << QSize(64, 32) << QSize(64, 32) << QString("png") << false;
    QTest::newRow("oriented-native-jpeg") << QByteArray("JPEG") << QSize(64, 32) << QSize(32, 64) << QString("bin") << true;
    QTest::newRow("oriented-large-jpeg") << QByteArray("JPEG") << QSize(2048, 1024) << QSize(640, 1280) << QString("png") << true;
    QTest::newRow("bounded-svg-render") << QByteArray("SVG") << QSize(32, 24) << QSize(1280, 960) << QString("png") << false;
}

void TestStudio::boundsPreviewImageTransfers()
{
    QFETCH(QByteArray, format);
    QFETCH(QSize, original);
    QFETCH(QSize, expected);
    QFETCH(QString, suffix);
    QFETCH(bool, rotated);
    QTemporaryDir scratch;
    QVERIFY(scratch.isValid());
    QByteArray encoded;
    QImage source(original, QImage::Format_ARGB32);
    source.fill(Qt::red);
    if (format == "SVG") {
        encoded = "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"32\" height=\"24\"><rect width=\"32\" height=\"24\" fill=\"red\"/></svg>";
    } else {
        QPainter painter(&source);
        painter.fillRect(original.width() / 2, 0, original.width() - original.width() / 2, original.height(), Qt::blue);
        painter.end();
        if (format == "PNG") source.setPixelColor(0, 0, QColor(30, 80, 120, 128));
        QBuffer buffer(&encoded);
        QVERIFY(buffer.open(QIODevice::WriteOnly));
        QVERIFY(source.save(&buffer, format.constData()));
        if (rotated) {
            // EXIF orientation 6: rotate the decoded source clockwise by 90 degrees.
            const QByteArray exif = QByteArray::fromHex("45786966000049492a0008000000010012010300010000000600000000000000");
            QByteArray app1("\xff\xe1", 2);
            app1 += char((exif.size() + 2) >> 8);
            app1 += char((exif.size() + 2) & 255);
            app1 += exif;
            encoded.insert(2, app1);
        }
    }
    const QString path = scratch.filePath("source." + suffix);
    { QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly)); QCOMPARE(file.write(encoded), encoded.size()); }
    QProcess helper;
    QSignalSpy finished(&helper, &QProcess::finished);
    helper.start(QDir(QCoreApplication::applicationDirPath()).absoluteFilePath("../../build/bin/omafiles-preview"), {path});
    QTRY_VERIFY2(!finished.isEmpty(), qPrintable(helper.errorString()));
    QCOMPARE(helper.exitStatus(), QProcess::NormalExit);
    QCOMPARE(helper.exitCode(), 0);
    const QByteArray frame = helper.readAllStandardOutput();
    QVERIFY2(frame.size() >= 4, helper.readAllStandardError().constData());
    const quint32 packetSize = qFromBigEndian<quint32>(frame.constData());
    QVERIFY(packetSize <= previewPacketLimit);
    QCOMPARE(frame.size(), qsizetype(packetSize) + 4);
    PreviewPacket packet;
    QVERIFY(readPreviewPacket(frame.mid(4), &packet));
    QCOMPARE(packet.kind, QStringLiteral("image"));
    QCOMPARE(packet.text, QStringLiteral("%1 × %2").arg(original.width()).arg(original.height()));
    QCOMPARE(packet.image.size(), expected);
    QCOMPARE(packet.image.sizeInBytes(), qint64(expected.width()) * expected.height() * 4);
    QVERIFY(!packet.playing);
    QVERIFY(!packet.loading);
    if (expected == original && format != "JPEG" && format != "SVG")
        QCOMPARE(packet.image, source);
    if (rotated) {
        const QColor top = packet.image.pixelColor(expected.width()/2, expected.height()/4);
        const QColor bottom = packet.image.pixelColor(expected.width()/2, 3*expected.height()/4);
        QVERIFY(top.red() > 245 && top.blue() < 10);
        QVERIFY(bottom.blue() > 245 && bottom.red() < 10);
    }
    if (format == "SVG") QCOMPARE(packet.image.pixelColor(16, 12), QColor(Qt::red));
    QFile unchanged(path);
    QVERIFY(unchanged.open(QIODevice::ReadOnly));
    QCOMPARE(unchanged.readAll(), encoded);
    QCOMPARE(QDir(scratch.path()).entryList(QDir::Files).size(), 1);
}

void TestStudio::previewsFilesWithinTheirBudgets()
{
    QTemporaryDir scratch;
    QVERIFY(scratch.isValid());
    const auto write = [&](const QString &name, const QByteArray &bytes) {
        QFile file(scratch.filePath(name));
        if (!file.open(QIODevice::WriteOnly)) return QString();
        if (file.write(bytes) != bytes.size()) return QString();
        return file.fileName();
    };
    Preview preview;
    const QString text = write("text.txt", QByteArray(70000, 'x'));
    QVERIFY(!text.isEmpty());
    preview.setPath(text);
    QTRY_COMPARE(preview.kind(), QStringLiteral("text"));
    QVERIFY(!preview.loading());
    QVERIFY(preview.text().contains("Preview truncated"));
    QVERIFY(preview.text().size() < 4200);
    preview.setPath(write("multiline.txt", (QByteArray(999, 'x') + '\n').repeated(70)));
    QTRY_COMPARE(preview.kind(), QStringLiteral("text"));
    QVERIFY(preview.text().contains("Preview truncated"));
    QVERIFY(preview.text().size() < 66000);

    QImage red(3000, 1500, QImage::Format_RGB32);
    red.fill(Qt::red);
    const QString image = scratch.filePath("red.png");
    QVERIFY(red.save(image));
    preview.setPath(image);
    QTRY_COMPARE(preview.kind(), QStringLiteral("image"));
    QVERIFY(preview.hasImage());
    QImage canvas(200, 200, QImage::Format_ARGB32);
    canvas.fill(Qt::transparent);
    preview.setSize(canvas.size());
    QPainter painter(&canvas);
    preview.paint(&painter);
    painter.end();
    QCOMPARE(canvas.pixelColor(100, 100), QColor(Qt::red));

    const QString pdf = scratch.filePath("pages.pdf");
    {
        QPdfWriter writer(pdf);
        QPainter page(&writer);
        page.fillRect(page.viewport(), Qt::red);
        QVERIFY(writer.newPage());
        page.fillRect(page.viewport(), Qt::blue);
    }
    preview.setPath(pdf);
    QTRY_COMPARE(preview.kind(), QStringLiteral("pdf"));
    QCOMPARE(preview.text(), QStringLiteral("Page 1 of 2"));
    QVERIFY(preview.hasImage());
    canvas.fill(Qt::transparent);
    painter.begin(&canvas); preview.paint(&painter); painter.end();
    QCOMPARE(canvas.pixelColor(100, 100), QColor(Qt::red));
    const QString textPdf = scratch.filePath("text.pdf");
    {
        QPdfWriter writer(textPdf);
        QPainter page(&writer);
        page.drawText(QPoint(100, 100), "Black text on paper");
    }
    preview.setPath(textPdf);
    QTRY_COMPARE(preview.kind(), QStringLiteral("pdf"));
    canvas.fill(Qt::transparent);
    painter.begin(&canvas); preview.paint(&painter); painter.end();
    QCOMPARE(canvas.pixelColor(100, 150), QColor(Qt::white));

    QByteArray archive;
    for (int i = 0; i < 510; ++i) {
        QByteArray header(512, '\0');
        const QByteArray name = i == 0 ? QByteArray("../../never-extracted") : QByteArray("entry-") + QByteArray::number(i);
        std::copy(name.begin(), name.end(), header.begin());
        std::snprintf(header.data() + 100, 8, "%07o", 0644);
        std::snprintf(header.data() + 124, 12, "%011o", 0);
        header[156] = '0';
        std::copy_n("ustar", 5, header.data() + 257);
        std::fill_n(header.data() + 148, 8, ' ');
        unsigned sum = 0;
        for (unsigned char byte : header) sum += byte;
        std::snprintf(header.data() + 148, 8, "%06o", sum);
        header[155] = ' ';
        archive += header;
    }
    archive += QByteArray(1024, '\0');
    const QString tar = write("entries.tar", archive);
    preview.setPath(tar);
    QTRY_COMPARE(preview.kind(), QStringLiteral("archive"));
    QVERIFY(preview.text().contains("../../never-extracted"));
    QVERIFY(preview.text().contains("entry-499"));
    QVERIFY(!preview.text().contains("entry-500"));
    QVERIFY(preview.text().contains("Archive listing truncated"));
    QCOMPARE(QDir(scratch.path()).entryList(QDir::Files).size(), 6);
    preview.setPath(write("exactly-500.tar", archive.left(500 * 512) + QByteArray(1024, '\0')));
    QTRY_COMPARE(preview.kind(), QStringLiteral("archive"));
    QVERIFY(preview.text().contains("entry-499"));
    QVERIFY(!preview.text().contains("truncated"));

    const QByteArray wave = silentWave();
    preview.setPath(write("silence.wav", wave));
    QTRY_COMPARE(preview.kind(), QStringLiteral("audio"));
    QVERIFY(!preview.playing());
    bool playbackAcknowledged = false;
    connect(&preview, &Preview::changed, &preview, [&] {
        playbackAcknowledged |= preview.playing();
    });
    preview.togglePlayback();
    QTRY_VERIFY2(playbackAcknowledged, qPrintable(preview.kind() + ": " + preview.text()));
    preview.pause();
    QTRY_VERIFY(!preview.playing());
    preview.seek(5);
    QTRY_COMPARE(preview.text(), QStringLiteral("Paused · 0:04 / 0:04"));

    preview.setPath(write("huge.svg", "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"100000\" height=\"100000\"><rect width=\"100000\" height=\"100000\" fill=\"red\"/></svg>"));
    QTRY_COMPARE(preview.kind(), QStringLiteral("error"));
    QVERIFY(preview.text().contains("megapixel"));

    const QString oversized = scratch.filePath("large.pdf");
    { QFile file(oversized); QVERIFY(file.open(QIODevice::WriteOnly)); file.write("%PDF-1.7\n"); QVERIFY(file.resize(17 * 1024 * 1024)); }
    preview.setPath(oversized);
    QTRY_COMPARE(preview.kind(), QStringLiteral("error"));
    QVERIFY(preview.text().contains("16 MiB"));
    preview.setPath(scratch.path());
    QTRY_VERIFY(preview.text().contains("regular files"));
    preview.setPath(scratch.filePath("missing"));
    QTRY_VERIFY(preview.text().contains("Cannot read"));
    preview.setPath(write("unknown.bin", QByteArray("\0\x01\xff", 3)));
    QTRY_COMPARE(preview.kind(), QStringLiteral("error"));
    QVERIFY(!preview.hasImage());
    QVERIFY(!preview.loading());
}

void TestStudio::cancelsPreviewWorkAndBoundsHelpers()
{
    QTemporaryDir scratch;
    QVERIFY(scratch.isValid());
    const QString helper = scratch.filePath("helper");
    QFile script(helper);
    QVERIFY(script.open(QIODevice::WriteOnly));
    script.write("#!/usr/bin/python3\nimport os,sys,time,struct\nfrom pathlib import Path\np=Path(sys.argv[1]);pid=Path(str(p)+'.pid');pid.touch()\nif p.name=='first':\n while not Path(str(p)+'.release').exists():time.sleep(0.001)\npid.write_text(str(os.getpid()))\nif p.name=='memory': data=bytearray(600*1024*1024)\nif p.name=='packet': sys.stdout.buffer.write(struct.pack('>I',9*1024*1024));sys.stdout.buffer.flush()\nif p.name=='exit':\n payload=struct.pack('>I',10)+'audio'.encode('utf-16-be')+struct.pack('>I',10)+'ready'.encode('utf-16-be')+struct.pack('>BBiiI',0,0,0,0,0xffffffff)\n sys.stdout.buffer.write(struct.pack('>I',len(payload))+payload);sys.stdout.buffer.flush();os._exit(3)\ntime.sleep(30)\n");
    script.close();
    QVERIFY(script.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
    const QString old = setPreviewHelper(helper);
    const auto restore = qScopeGuard([&] { setPreviewHelper(old); });
    Preview preview;
    const QString first = scratch.filePath("first");
    preview.setPath(first);
    const auto helperPid = [](const QString &path) {
        QFile file(path + ".pid");
        return file.open(QIODevice::ReadOnly) ? file.readAll().toInt() : 0;
    };
    QTRY_VERIFY(QFileInfo::exists(first + ".pid"));
    QTimer::singleShot(0, &preview, [first] {
        QFile release(first + ".release");
        QVERIFY(release.open(QIODevice::WriteOnly));
    });
    QTRY_VERIFY(helperPid(first) > 0);
    const int oldPid = helperPid(first);
    QVERIFY(oldPid > 0);
    preview.setPath(scratch.filePath("second"));
    QTRY_VERIFY(helperPid(scratch.filePath("second")) > 0);
    QTRY_VERIFY(::kill(oldPid, 0) != 0);
    preview.setPath("");
    QVERIFY(!preview.loading());
    QVERIFY(preview.kind().isEmpty());
    preview.setPath(scratch.filePath("memory"));
    QTRY_VERIFY(preview.text().contains("512 MiB"));
    QVERIFY(!preview.playing());
    preview.setPath(scratch.filePath("packet"));
    QTRY_VERIFY(preview.text().contains("too much data"));
    preview.setPath(scratch.filePath("exit"));
    QTRY_VERIFY(preview.text().contains("stopped unexpectedly"));
    preview.setPath(scratch.filePath("timeout"));
    QTRY_VERIFY(preview.text().contains("timed out"));
    preview.setPath("");
    QVERIFY(preview.text().isEmpty());
}

void TestStudio::navigatesQuickPreviewFromEveryFileView()
{
    auto *folder = qobject_cast<FolderModel *>(filesRoot()->property("folder").value<QObject *>());
    QVERIFY(folder);
    const int firstFile = folder->previewRow(-1, 1);
    QVERIFY(firstFile > 0);
    QCOMPARE(folder->kindAt(firstFile), QStringLiteral("file"));
    QCOMPARE(folder->previewRow(firstFile, -1), -1);
    auto *sheet = itemsNamed("previewSheet").first();
    Preview *preview = nullptr;
    for (const QString &mode : {QStringLiteral("list"), QStringLiteral("details"), QStringLiteral("icons")}) {
        tab()->setViewMode(mode); settle(); focusRow("one.txt");
        const QString directory = cwd();
        const int first = cursor(), next = folder->previewRow(first, 1);
        QVERIFY(next >= 0);
        const int marked = folder->markedCount();
        key('i');
        QTRY_VERIFY(sheet->isVisible());
        preview = qobject_cast<Preview *>(itemsNamed("quickPreview").first());
        QVERIFY(preview);
        QTRY_COMPARE(preview->kind(), QStringLiteral("text"));
        QCOMPARE(preview->path(), m_tree.filePath("one.txt"));
        key('j');
        QTRY_COMPARE(cursor(), next);
        QTRY_COMPARE(preview->path(), folder->pathAt(next));
        key('k'); QCOMPARE(cursor(), first);
        key(Qt::Key_Right); QCOMPARE(cursor(), next);
        key(Qt::Key_Left); QCOMPARE(cursor(), first);
        QCOMPARE(cwd(), directory);
        QCOMPARE(folder->markedCount(), marked);
        key('i'); QTRY_VERIFY(!sheet->isVisible());
        key('j'); QVERIFY(cursor() != first);
        focusRow("one.txt"); key('i'); key(Qt::Key_Escape);
        QTRY_VERIFY(!sheet->isVisible());
    }
    focusRow("one.txt");
    key('m');
    QVERIFY(chooseInMenu(QStringLiteral("fileMenu"), QStringLiteral("Quick preview")));
    QTRY_VERIFY(sheet->isVisible());
    key(Qt::Key_Escape);
    key(':');
    for (char c : QByteArray("Quick preview")) key(c);
    key(Qt::Key_Return);
    QTRY_VERIFY(sheet->isVisible());
    key(Qt::Key_Escape);
}

void TestStudio::traversesPreviewControlsAndRestoresCoveredFocus()
{
    const QSize original = window()->size();
    const auto restore = qScopeGuard([&] { window()->resize(original); });
    window()->resize(400, 250);
    focusRow("one.txt"); key('i');
    auto *sheet = itemsNamed("previewSheet").first();
    auto *close = itemsNamed("previewClose").first();
    bool reachedClose = false;
    for (int i = 0; i < 12 && !reachedClose; ++i) {
        key(Qt::Key_Tab);
        reachedClose = window()->activeFocusItem() == close;
    }
    QVERIFY(reachedClose);
    QVERIFY(QMetaObject::invokeMethod(window(), "takeFocus"));
    QTRY_COMPARE(window()->activeFocusItem(), close);
    QVERIFY(QRectF(QPointF(), window()->size()).contains(close->mapToScene(QPointF(close->width()/2, close->height()/2))));
    FileManager manager;
    manager.setWorkspace(m_workspace);
    manager.ShowItemProperties({QUrl::fromLocalFile(m_tree.filePath("one.txt")).toString()}, "");
    settle();
    QVERIFY(window()->activeFocusItem() != close);
    key(Qt::Key_Escape);
    QTRY_COMPARE(window()->activeFocusItem(), close);
    key(Qt::Key_Backtab, Qt::ShiftModifier);
    QVERIFY(window()->activeFocusItem() != close);
    key(Qt::Key_Tab);
    QTRY_COMPARE(window()->activeFocusItem(), close);
    key(Qt::Key_Return);
    QTRY_VERIFY(!sheet->isVisible());
    key('i'); close->forceActiveFocus(); key(Qt::Key_Space);
    QTRY_VERIFY(!sheet->isVisible());
    key('?'); QTRY_VERIFY(itemsNamed("helpSheet").first()->isVisible()); key(Qt::Key_Escape);
}

void TestStudio::pausesPreviewMediaWhenCovered()
{
    ScriptedPlayback playback;
    QVERIFY(playback.isValid());
    QTemporaryDir scratch;
    QFile file(scratch.filePath("silence.wav"));
    QVERIFY(file.open(QIODevice::WriteOnly));
    const auto wave = silentWave();
    QCOMPARE(file.write(wave), wave.size());
    file.close();
    auto *sheet = itemsNamed("previewSheet").first();
    const auto close = qScopeGuard([&] { QMetaObject::invokeMethod(sheet, "close"); });
    QVERIFY(QMetaObject::invokeMethod(sheet, "openFor", Q_ARG(QVariant, file.fileName())));
    auto *preview = qobject_cast<Preview *>(itemsNamed("quickPreview").first());
    QVERIFY(preview);
    QTRY_COMPARE(preview->kind(), QStringLiteral("audio"));
    QVERIFY(!preview->playing());
    key('p'); QTRY_VERIFY(preview->playing());
    FileManager manager;
    manager.setWorkspace(m_workspace);
    manager.ShowItemProperties({QUrl::fromLocalFile(file.fileName()).toString()}, "");
    QTRY_VERIFY(!sheet->property("frontmost").toBool());
    QTRY_VERIFY(!preview->playing());
    key(Qt::Key_Escape);
    QTRY_VERIFY(sheet->property("frontmost").toBool());
    QVERIFY(!preview->playing());
    key(']'); QTRY_COMPARE(preview->text(), QStringLiteral("Paused · 0:04 / 0:04"));
    QVERIFY(!preview->playing());
    key('['); QTRY_COMPARE(preview->text(), QStringLiteral("Paused · 0:00 / 0:04"));
    QVERIFY(!preview->playing());
    key('p'); QTRY_VERIFY(preview->playing());
    key(Qt::Key_Escape);
    QTRY_VERIFY(!sheet->isVisible());
    QTRY_VERIFY(preview->path().isEmpty());
    QVERIFY(!preview->playing());
}

static bool hasFocusOutline(QQuickWindow *window, QQuickItem *frame, const QColor &color)
{
    const QImage image = window->grabWindow();
    const QRect rectangle = frame->mapRectToScene(frame->boundingRect()).toAlignedRect().intersected(image.rect());
    for (int y = rectangle.top(); y < std::min(rectangle.top() + 4, rectangle.bottom()); ++y) {
        int matching = 0;
        for (int x = rectangle.left(); x <= rectangle.right(); ++x)
            if (image.pixelColor(x, y) == color) ++matching;
        if (matching > rectangle.width() / 2) return true;
    }
    return false;
}

void TestStudio::traversesReceiptControls_data()
{
    QTest::addColumn<QString>("underlay");
    QTest::addColumn<QSize>("size");
    QTest::addColumn<bool>("reverse");
    QTest::addColumn<int>("activation");
    for (const QString underlay : {"list", "help", "rename", "tags", "preview", "default", "incomplete", "receipt"})
        for (const QSize &size : {QSize(1200, 800), QSize(420, 260)})
            for (bool reverse : {false, true})
                for (int activation : {Qt::Key_Return, Qt::Key_Space}) {
                    const auto name = QString("%1-%2-%3-%4").arg(underlay).arg(size.width())
                        .arg(reverse ? "backtab" : "tab").arg(activation);
                    QTest::newRow(qPrintable(name)) << underlay << size << reverse << activation;
                }
}

void TestStudio::traversesReceiptControls()
{
    QFETCH(QString, underlay);
    QFETCH(QSize, size);
    QFETCH(bool, reverse);
    QFETCH(int, activation);
    const QSize original = window()->size();
    const auto restore = qScopeGuard([&] { window()->resize(original); });
    window()->resize(size);
    focusRow("one.txt");
    if (underlay == "help") key('?');
    else if (underlay == "rename") { key('r'); key('x'); }
    else if (underlay == "tags") { key('t'); key('a'); }
    else if (underlay == "preview") key('i');
    QPointer<QQuickItem> origin = window()->activeFocusItem();
    QVERIFY(origin);
    const QString draft = origin->property("text").toString();
    if (underlay == "default" || underlay == "incomplete")
        m_index->defaultManagerFinished(underlay == "default", QString("Read-only receipt\n").repeated(80));
    else if (underlay == "receipt")
        QVERIFY(QMetaObject::invokeMethod(window(), "showReceipt", Q_ARG(QVariant, "Receipt"),
            Q_ARG(QVariant, QString("Read-only receipt\n").repeated(80))));
    else {
        FileManager manager;
        manager.setWorkspace(m_workspace);
        manager.ShowItemProperties({QUrl::fromLocalFile(m_tree.filePath("one.txt")).toString()}, "");
    }
    settle();
    QPointer<QObject> dialog = window()->findChild<QObject *>("propertiesDialog");
    QVERIFY(dialog);
    const auto dismiss = qScopeGuard([&] {
        if (dialog) QMetaObject::invokeMethod(dialog, "close");
    });
    auto *text = dialog->findChild<QQuickItem *>("propertyText");
    auto *close = dialog->findChild<QQuickItem *>("receiptClose");
    QVERIFY(text && close);
    QTRY_COMPARE(window()->activeFocusItem(), text);
    auto *frame = text->property("focusFrame").value<QQuickItem *>();
    QVERIFY(frame);
    const QColor accent = m_theme->property("accent").value<QColor>();
    QTRY_VERIFY(hasFocusOutline(window(), frame, accent));
    const QString contents = text->property("text").toString();
    key('x'); QCOMPARE(text->property("text").toString(), contents);
    key('a', Qt::ControlModifier);
    const QString selection = text->property("selectedText").toString();
    QVERIFY(!selection.isEmpty());
    const auto tabKey = reverse ? Qt::Key_Backtab : Qt::Key_Tab;
    const auto modifiers = reverse ? Qt::ShiftModifier : Qt::NoModifier;
    key(tabKey, modifiers);
    QTRY_COMPARE(window()->activeFocusItem(), close);
    QTRY_VERIFY(hasFocusOutline(window(), close, accent));
    QVERIFY(QRectF(QPointF(), window()->size()).contains(close->mapToScene(QPointF(close->width()/2, close->height()/2))));
    key(tabKey, modifiers);
    QTRY_COMPARE(window()->activeFocusItem(), text);
    QCOMPARE(text->property("selectedText").toString(), selection);
    key(tabKey, modifiers);
    QTRY_COMPARE(window()->activeFocusItem(), close);
    key(static_cast<Qt::Key>(activation));
    QTRY_VERIFY(!dialog || !dialog->property("visible").toBool());
    QTRY_COMPARE(window()->activeFocusItem(), origin.data());
    QCOMPARE(origin->property("text").toString(), draft);
    key(Qt::Key_Escape); key(Qt::Key_Escape);
    QVERIFY(!window()->property("blocked").toBool());
    const int before = cursor(); key('j'); QCOMPARE(cursor(), before + 1);
}

void TestStudio::traversesRefusedPermissionSheets_data()
{
    QTest::addColumn<QSize>("size");
    QTest::addColumn<bool>("reverse");
    QTest::addColumn<int>("activation");
    for (const QSize &size : {QSize(1200, 800), QSize(420, 260)})
        for (bool reverse : {false, true})
            for (int activation : {Qt::Key_Return, Qt::Key_Space})
                QTest::newRow(qPrintable(QString("%1-%2-%3").arg(size.width()).arg(reverse).arg(activation)))
                    << size << reverse << activation;
}

void TestStudio::traversesRefusedPermissionSheets()
{
    QFETCH(QSize, size);
    QFETCH(bool, reverse);
    QFETCH(int, activation);
    const QSize original = window()->size();
    const auto restore = qScopeGuard([&] { window()->resize(original); });
    window()->resize(size);
    auto *sheet = itemsNamed("modeSheet").first();
    const QVariant info = QVariantMap{{"link", true}, {"mine", true}, {"permissions", 0644}};
    QVERIFY(QMetaObject::invokeMethod(sheet, "openFor",
        Q_ARG(QVariant, QStringList{m_tree.filePath("one.txt")}), Q_ARG(QVariant, info)));
    settle();
    QVERIFY(!sheet->property("refusal").toString().isEmpty());
    const auto runs = verbRuns();
    auto *scroll = sheet->findChild<QQuickItem *>("overlayScroll");
    QVERIFY(scroll);
    QQuickItem *focused = nullptr;
    for (int step = 0; step < 3; ++step) {
        key(reverse ? Qt::Key_Backtab : Qt::Key_Tab, reverse ? Qt::ShiftModifier : Qt::NoModifier);
        focused = window()->activeFocusItem();
        QVERIFY(focused && focused->isEnabled() && focused->isVisible());
        QVERIFY(sheet->isAncestorOf(focused));
        QVERIFY(focused == scroll || focused->property("label").toString() == "cancel");
        QTRY_VERIFY(hasFocusOutline(window(), focused, m_theme->property("accent").value<QColor>()));
        if (focused->property("label").toString() == "cancel") break;
    }
    QVERIFY(focused);
    QCOMPARE(focused->property("label").toString(), QStringLiteral("cancel"));
    key(static_cast<Qt::Key>(activation));
    QTRY_VERIFY(!sheet->isVisible());
    QCOMPARE(verbRuns(), runs);
}

void TestStudio::selectsFocusedPreviewText_data()
{
    QTest::addColumn<QSize>("size");
    QTest::addColumn<bool>("longText");
    QTest::addColumn<int>("keyCode");
    QTest::addColumn<int>("modifiers");
    QTest::addColumn<int>("initial");
    QTest::addColumn<int>("destination");
    struct Gesture { const char *name; int key; int modifiers; int initial; int destination; };
    const int shift = Qt::ShiftModifier, word = Qt::ControlModifier | Qt::ShiftModifier;
    const Gesture gestures[] = {
        {"character-right", Qt::Key_Right, shift, 0, 1},
        {"character-left", Qt::Key_Left, shift, 1, 0},
        {"word-right", Qt::Key_Right, word, 0, 6},
        {"word-left", Qt::Key_Left, word, 6, 0},
        {"line-down", Qt::Key_Down, shift, 0, 17},
        {"line-up", Qt::Key_Up, shift, 17, 0},
        {"line-start", Qt::Key_Home, shift, 10, 0},
        {"line-end", Qt::Key_End, shift, 0, 16},
        {"document-start", Qt::Key_Home, word, 20, 0},
        {"document-end", Qt::Key_End, word, 0, -1},
        {"caret-document-start", Qt::Key_Home, Qt::ControlModifier, 20, 0},
        {"caret-document-end", Qt::Key_End, Qt::ControlModifier, 0, -1}
    };
    for (const QSize &size : {QSize(1180, 720), QSize(420, 720)})
        for (bool longText : {false, true})
            for (const auto &gesture : gestures)
                QTest::newRow(qPrintable(QString("%1-%2-%3").arg(size.width()).arg(longText).arg(gesture.name)))
                    << size << longText << gesture.key << gesture.modifiers << gesture.initial << gesture.destination;
}

void TestStudio::selectsFocusedPreviewText()
{
    QFETCH(QSize, size);
    QFETCH(bool, longText);
    QFETCH(int, keyCode);
    QFETCH(int, modifiers);
    QFETCH(int, initial);
    QFETCH(int, destination);
    QTemporaryDir fixture(m_tree.filePath("preview-selection-XXXXXX"));
    QVERIFY(fixture.isValid());
    const QString content = QStringLiteral("alpha beta gamma\nsecond delta omega\nthird line\n")
        .repeated(longText ? 80 : 1);
    for (const QString &name : {QStringLiteral("a.txt"), QStringLiteral("b.txt"), QStringLiteral("c.txt")}) {
        QFile file(fixture.filePath(name));
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write(name == "b.txt" ? content.toUtf8() : QByteArray("other file\n")),
                 name == "b.txt" ? content.toUtf8().size() : qsizetype(11));
    }
    const QSize original = window()->size();
    const auto restore = qScopeGuard([&] {
        for (auto *sheet : itemsNamed("previewSheet")) QMetaObject::invokeMethod(sheet, "close");
        window()->resize(original);
        tab()->setCwd(m_tree.path());
    });
    window()->resize(size);
    tab()->setCwd(fixture.path()); settle();
    focusRow("b.txt");
    const int fileCursor = cursor();
    key('i');
    auto *sheet = itemsNamed("previewSheet").first();
    auto *preview = qobject_cast<Preview *>(itemsNamed("quickPreview").first());
    QVERIFY(preview);
    QTRY_COMPARE(preview->kind(), QStringLiteral("text"));
    QTRY_COMPARE(preview->text(), content);
    auto *text = itemsNamed("previewText").first();
    for (int stop = 0; stop < 8 && window()->activeFocusItem() != text; ++stop) key(Qt::Key_Tab);
    QTRY_COMPARE(window()->activeFocusItem(), text);
    QVERIFY(text->setProperty("cursorPosition", initial));
    if (keyCode == Qt::Key_Right && modifiers == Qt::ShiftModifier) {
        key(Qt::Key_Home, Qt::ControlModifier);
        QCOMPARE(text->property("cursorPosition").toInt(), 0);
    }
    const QString path = preview->path();
    key(static_cast<Qt::Key>(keyCode), Qt::KeyboardModifiers(modifiers));
    QCOMPARE(cursor(), fileCursor);
    QCOMPARE(preview->path(), path);
    QCOMPARE(window()->activeFocusItem(), text);
    const int end = destination < 0 ? content.size() : destination;
    QCOMPARE(text->property("cursorPosition").toInt(), end);
    const bool extending = modifiers & Qt::ShiftModifier;
    QCOMPARE(text->property("selectionStart").toInt(), extending ? qMin(initial, end) : end);
    QCOMPARE(text->property("selectionEnd").toInt(), extending ? qMax(initial, end) : end);
    const QString selected = text->property("selectedText").toString().replace(QChar::ParagraphSeparator, QLatin1Char('\n'));
    QCOMPARE(selected, extending ? content.mid(qMin(initial, end), qAbs(end - initial)) : QString());
    auto *frame = text->property("focusFrame").value<QQuickItem *>();
    QVERIFY(frame);
    QTRY_VERIFY(hasFocusOutline(window(), frame, m_theme->property("accent").value<QColor>()));
    if (extending) {
        key('c', Qt::ControlModifier);
        QCOMPARE(QGuiApplication::clipboard()->text(), selected);
    }
    key('a', Qt::ControlModifier); key('c', Qt::ControlModifier);
    QCOMPARE(QGuiApplication::clipboard()->text(), content);
    QCOMPARE(text->property("text").toString(), content);
    key(Qt::Key_Right); QTRY_COMPARE(cursor(), fileCursor + 1);
    QTRY_COMPARE(preview->path(), fixture.filePath("c.txt"));
    key(Qt::Key_Left); QTRY_COMPARE(cursor(), fileCursor);
    QTRY_COMPARE(preview->text(), content);
    key('k'); QTRY_COMPARE(preview->path(), fixture.filePath("a.txt"));
    key('j'); QTRY_COMPARE(preview->path(), path);
    key(Qt::Key_Escape); QTRY_VERIFY(!sheet->isVisible());
    QVERIFY(!window()->property("blocked").toBool());
}

void TestStudio::leavesSelectedPreviewText_data()
{
    QTest::addColumn<bool>("reverse");
    QTest::newRow("tab") << false;
    QTest::newRow("backtab") << true;
}

void TestStudio::leavesSelectedPreviewText()
{
    QFETCH(bool, reverse);
    focusRow("one.txt"); key('i');
    auto *text = itemsNamed("previewText").first();
    auto *preview = qobject_cast<Preview *>(itemsNamed("quickPreview").first());
    QVERIFY(preview);
    QTRY_VERIFY(!preview->loading());
    text->forceActiveFocus();
    QTRY_COMPARE(window()->activeFocusItem(), text);
    key('a', Qt::ControlModifier);
    const QString selection = text->property("selectedText").toString();
    QVERIFY(!selection.isEmpty());
    auto *frame = text->property("focusFrame").value<QQuickItem *>();
    QVERIFY(frame);
    QTRY_VERIFY(hasFocusOutline(window(), frame, m_theme->property("accent").value<QColor>()));
    key(reverse ? Qt::Key_Backtab : Qt::Key_Tab, reverse ? Qt::ShiftModifier : Qt::NoModifier);
    auto *close = itemsNamed("previewClose").first();
    QTRY_COMPARE(window()->activeFocusItem(), reverse ? close : frame);
    QCOMPARE(text->property("selectedText").toString(), selection);
    close->forceActiveFocus();
    key(Qt::Key_Tab);
    QTRY_COMPARE(window()->activeFocusItem(), text);
    QCOMPARE(text->property("selectedText").toString(), selection);
    key(Qt::Key_Tab);
    QTRY_COMPARE(window()->activeFocusItem(), frame);
    key(Qt::Key_Backtab, Qt::ShiftModifier);
    QTRY_COMPARE(window()->activeFocusItem(), text);
    key(Qt::Key_Backtab, Qt::ShiftModifier);
    QTRY_COMPARE(window()->activeFocusItem(), close);
    auto *focus = window()->activeFocusItem();
    QVERIFY(focus);
    QVERIFY(QRectF(QPointF(), window()->size()).contains(focus->mapToScene(QPointF(focus->width()/2, focus->height()/2))));
    key(Qt::Key_Escape);
    QVERIFY(!itemsNamed("previewSheet").first()->isVisible());
}

void TestStudio::reportsPreviewFailureReasons_data()
{
    QTest::addColumn<QByteArray>("bytes");
    QTest::addColumn<QString>("expected");
    QTest::newRow("unreadable-image-header") << QByteArray("\x89PNG\r\n\x1a\nBROKEN", 14) << QString("Cannot read this image:");
    QByteArray png;
    QBuffer buffer(&png);
    QVERIFY(buffer.open(QIODevice::WriteOnly));
    QImage image(160, 100, QImage::Format_RGB32);
    image.fill(Qt::red);
    QVERIFY(image.save(&buffer, "PNG"));
    QTest::newRow("truncated-image-body") << png.left(png.size() / 2) << QString("Cannot read this image:");
    const QByteArray palettePng = QByteArray::fromBase64(
        "iVBORw0KGgoAAAANSUhEUgAAAKAAAABkAQMAAAA7eKnMAAAAIGNIUk0AAHomAACAhAAA+gAAAIDoAAB1MAAA6mAAADqYAAAXcJy6UTwAAAADUExURf8AABniCTcAAAAHdElNRQfqCgsVKAZIaWfgAAAAJXRFWHRkYXRlOmNyZWF0ZQAyMDI2LTEwLTExVDIxOjQwOjA2KzAwOjAw3TUGdgAAACV0RVh0ZGF0ZTptb2RpZnkAMjAyNi0xMC0xMVQyMTo0MDowNiswMDowMKxovsoAAAAodEVYdGRhdGU6dGltZXN0YW1wADIwMjYtMTAtMTFUMjE6NDA6MDYrMDA6MDD7fZ8VAAAAGElEQVRIx2NgGAWjYBSMglEwCkbBKCAMAAg0AAHiHsIcAAAAAElFTkSuQmCC");
    QTest::newRow("truncated-palette-image") << palettePng.left(palettePng.size() / 2)
        << QString("Cannot read this image: incomplete or unreadable image data.");
    const int payload = png.indexOf("IDAT") + 4;
    QVERIFY(payload > 4 && payload < png.size());
    png[payload] = char(0xff);
    QBuffer corrupted(&png);
    QVERIFY(corrupted.open(QIODevice::ReadOnly));
    QImageReader reader(&corrupted, "PNG");
    QVERIFY(reader.size().isValid());
    QVERIFY(reader.read().isNull());
    QTest::newRow("unreadable-image-body") << png << QString("Cannot read this image:");
    QTest::newRow("invalid-pdf") << QByteArray("%PDF-1.7\nBROKEN") << QString("Cannot read this PDF: invalid PDF data.");
    QTest::newRow("invalid-archive") << QByteArray("PK\x03\x04" "BROKEN", 10) << QString("Cannot list this archive:");
    QTest::newRow("unrecognized-format") << QByteArray("\0\x01\xff", 3) << QString("Cannot read this file as an archive:");
}

void TestStudio::reportsPreviewFailureReasons()
{
    QFETCH(QByteArray, bytes);
    QFETCH(QString, expected);
    QTemporaryDir scratch;
    QFile file(scratch.filePath("broken"));
    QVERIFY(file.open(QIODevice::WriteOnly));
    QCOMPARE(file.write(bytes), bytes.size());
    file.close();
    Preview preview;
    preview.setPath(file.fileName());
    QTRY_COMPARE(preview.kind(), QStringLiteral("error"));
    QVERIFY2(preview.text().startsWith(expected), qPrintable(preview.text()));
    QVERIFY(preview.text().size() > expected.size() || expected.endsWith('.'));
    QVERIFY2(!preview.text().contains("Unknown", Qt::CaseInsensitive), qPrintable(preview.text()));
    QVERIFY(!preview.text().contains("megapixel"));
    QVERIFY(!preview.text().contains("budget"));
    QVERIFY(!preview.text().contains("may"));
    QVERIFY(!preview.hasImage());
    QVERIFY(!preview.loading());
    const QString helper = setPreviewHelper(scratch.filePath("missing-helper"));
    const auto restore = qScopeGuard([&] { setPreviewHelper(helper); });
    Preview missing;
    missing.setPath(file.fileName());
    QTRY_COMPARE(missing.kind(), QStringLiteral("error"));
    QVERIFY2(missing.text().contains("No such file"), qPrintable(missing.text()));
    const QString blocked = scratch.filePath("blocked-helper");
    QFile script(blocked);
    QVERIFY(script.open(QIODevice::WriteOnly));
    script.write("#!/bin/sh\nexit 0\n"); script.close();
    QVERIFY(script.setPermissions(QFile::ReadOwner | QFile::WriteOwner));
    setPreviewHelper(blocked);
    Preview denied;
    denied.setPath(file.fileName());
    QTRY_COMPARE(denied.kind(), QStringLiteral("error"));
    QVERIFY2(denied.text().contains("Permission denied"), qPrintable(denied.text()));
    QVERIFY(!denied.text().contains("Install"));
    setPreviewHelper(helper);
    Preview absent;
    absent.setPath(scratch.filePath("missing-source"));
    QTRY_COMPARE(absent.kind(), QStringLiteral("error"));
    QVERIFY2(absent.text().contains("No such file"), qPrintable(absent.text()));
}


void TestStudio::opensRowMenusByTouch_data()
{
    QTest::addColumn<QString>("mode");
    QTest::addColumn<QString>("view");
    for (const QString &mode : {QString("list"), QString("details"), QString("icons")})
        for (const QString &view : {QString("directory"), QString("tag-filter"), QString("search"), QString("saved-view"), QString("facets")})
            QTest::newRow(qPrintable(mode + "-" + view)) << mode << view;
}

void TestStudio::opensRowMenusByTouch()
{
    QFETCH(QString, mode);
    QFETCH(QString, view);
    TabState *originalTab = tab();
    QTemporaryDir cache;
    const QByteArray previousCache = qgetenv("XDG_CACHE_HOME");
    qputenv("XDG_CACHE_HOME", cache.path().toUtf8());
    const QVector<Root> previousRoots = declareRoots({Root{"touch", m_tree.path(), false, {}}});
    const auto restore = qScopeGuard([&] {
        originalTab->setPattern({});
        declareRoots(previousRoots); qputenv("XDG_CACHE_HOME", previousCache);
    });
    QVERIFY(writeNames("touch", {"one.txt", "seven.txt"}, {}, nullptr, nullptr));
    if (view == "tag-filter") tab()->setSelectedTag("notes");
    else if (view == "search") tab()->setPattern("*.txt");
    else if (view == "saved-view") {
        key('v'); key(Qt::Key_Home); key(Qt::Key_Return);
    } else if (view == "facets") m_workspace->openTabIn(0, "tags", "");
    tab()->setViewMode(mode); settle();
    auto *menu = itemsNamed("fileMenu").first();
    auto *folder = qobject_cast<FolderModel *>(filesRoot()->property("folder").value<QObject *>());
    QVERIFY(folder);
    QTRY_VERIFY(!folder->property("loading").toBool());
    if (view != "facets") QTRY_VERIFY(folder->previewRow(-1, 1) >= 0);
    const int wanted = view == "facets" ? 0 : folder->previewRow(-1, 1);
    QVERIFY(wanted >= 0);
    QMetaObject::invokeMethod(filesRoot(), "setCursor", Q_ARG(QVariant, wanted)); settle();
    QTRY_VERIFY(visibleRow("fileRow", wanted));
    QQuickItem *row = visibleRow("fileRow", wanted);
    const QString directory = cwd();
    const int marked = folder->markedCount();
    openRequests().clear(); verbRuns().clear();
    QVERIFY(tapItem(window(), row, true)); settle();
    QVERIFY(menu->isVisible());
    QCOMPARE(cursor(), wanted);
    QCOMPARE(cwd(), directory);
    QCOMPARE(folder->markedCount(), marked);
    QVERIFY(openRequests().isEmpty()); QVERIFY(verbRuns().isEmpty());
    if (view != "facets") {
        QVERIFY(menuLabels("fileMenu").contains("Quick preview"));
        QQuickItem *action = nullptr;
        QList<QQuickItem *> queue{menu};
        while (!queue.isEmpty()) {
            auto *item = queue.takeFirst(); queue.append(item->childItems());
            if (item->isVisible() && item->property("text").toString() == "Quick preview") action = item;
        }
        QVERIFY(action);
        QVERIFY(tapItem(window(), action)); settle();
        QTRY_VERIFY(itemsNamed("previewSheet").first()->isVisible());
        key(Qt::Key_Escape);
    } else key(Qt::Key_Escape);
    QVERIFY(!menu->isVisible());
    QVERIFY(window()->activeFocusItem());
    key('m'); QVERIFY(menu->isVisible()); key(Qt::Key_Escape);
    key(Qt::Key_Menu); QVERIFY(menu->isVisible()); key(Qt::Key_Escape);
    key(Qt::Key_F10, Qt::ShiftModifier); QVERIFY(menu->isVisible()); key(Qt::Key_Escape);
}

void TestStudio::opensSidebarMenusByTouchAndKeyboard()
{
    focusRow("one.txt");
    for (const QString &kind : {QString("tag"), QString("view")}) {
        key(kind == "tag" ? 't' : 'v');
        key(Qt::Key_Home);
        window()->grabWindow();
        QTRY_VERIFY(visibleRow(kind + "Row", 0));
        QQuickItem *row = visibleRow(kind + "Row", 0);
        auto *menu = itemsNamed(kind + "Menu").first();
        const QString selected = tab()->selectedTag(), directory = cwd();
        QVERIFY(tapItem(window(), row, true)); settle();
        QVERIFY(menu->isVisible());
        QCOMPARE(cwd(), directory); QCOMPARE(tab()->selectedTag(), selected);
        key(Qt::Key_Escape);
        QCOMPARE(window()->property("focusRegion").toString(), kind == "tag" ? QString("tags") : QString("views"));
        for (int gesture = 0; gesture < 3; ++gesture) {
            if (gesture == 0) key('m');
            else if (gesture == 1) key(Qt::Key_Menu);
            else key(Qt::Key_F10, Qt::ShiftModifier);
            QVERIFY(menu->isVisible()); key(Qt::Key_Escape);
        }
        if (kind == "tag") {
            key('m'); QVERIFY(chooseInMenu("tagMenu", "Rename tag…"));
            QTRY_VERIFY(itemsNamed("tagSheet").first()->isVisible());
            key(Qt::Key_Escape);
        }
        key(Qt::Key_Escape);
    }
}

static QQuickItem *pointerControl(QQmlEngine *engine, QQuickWindow *window, qreal z)
{
    QQmlComponent component(engine, QUrl("qrc:/qml/FocusableControl.qml"));
    auto *item = qobject_cast<QQuickItem *>(component.create());
    if (!item) return nullptr;
    item->setParentItem(window->contentItem());
    item->setPosition(QPointF(20, 70));
    item->setSize(QSizeF(50, 40));
    item->setZ(z);
    return item;
}

void TestStudio::waitsForLayoutBeforeInjectingPointerInput_data()
{
    QTest::addColumn<bool>("pen");
    QTest::newRow("touch") << false;
    QTest::newRow("pen") << true;
}

void TestStudio::waitsForLayoutBeforeInjectingPointerInput()
{
    QFETCH(bool, pen);
    QQuickWindow scene;
    scene.resize(320, 200);
    scene.show();
    QVERIFY(QTest::qWaitForWindowExposed(&scene, asyncTimeout));
    QScopedPointer<QQuickItem> target(pointerControl(m_engine, &scene, 1));
    QScopedPointer<QQuickItem> oldSpot(pointerControl(m_engine, &scene, 0));
    QVERIFY(target && oldSpot);
    QSignalSpy activated(target.data(), SIGNAL(activated()));
    QSignalSpy staleInput(oldSpot.data(), SIGNAL(activated()));
    QVERIFY(activated.isValid() && staleInput.isValid());
    ScenePolish layout(scene.contentItem());
    layout.action = [&] { target->setX(200); };
    QVERIFY(QQuickTest::qWaitForPolish(&scene, asyncTimeout));
    const QPointF oldCenter = target->mapToScene(QPointF(25, 20));
    const QPointer<QQuickItem> alive(target.data());
    InputPressObserver input;
    input.polished = &layout.completed;
    scene.installEventFilter(&input);
    QVERIFY(!layout.completed);
    layout.polish();
    QVERIFY(QQuickTest::qIsPolishScheduled(&layout));
    QVERIFY(tapItem(&scene, target.data(), false, pen));
    QVERIFY(alive);
    QVERIFY(layout.completed);
    QVERIFY(!QRectF(target->mapToScene(QPointF()), target->size()).contains(oldCenter));
    QVERIFY(input.presses > 0);
    QVERIFY(input.allAfterPolish);
    QCOMPARE(activated.count(), 1);
    QCOMPARE(staleInput.count(), 0);
}

void TestStudio::activatesFooterHintsByTouchAndKeyboard()
{
    const auto hint = [&](const QString &label) -> QQuickItem * {
        for (auto *item : itemsNamed("hintAction"))
            if (item->isVisible() && item->isEnabled()
                    && item->property("tooltipCaption").toString() == label) return item;
        return nullptr;
    };
    const QSize original = window()->size();
    const auto resize = qScopeGuard([&] { window()->resize(original); });
    window()->resize(420, 300); settle();
    focusRow("one.txt");
    auto *folder = qobject_cast<FolderModel *>(filesRoot()->property("folder").value<QObject *>());
    QVERIFY(folder); folder->clearMarks();
    auto *menu = itemsNamed("fileMenu").first();
    for (int activation = 0; activation < 3; ++activation) {
        auto *button = hint("menu"); QVERIFY(button);
        window()->update(); QTest::qWait(30);
        const QPoint corner = button->mapToScene(QPointF(1, 1)).toPoint();
        const QImage frame = window()->grabWindow();
        QVERIFY(!frame.isNull());
        QVERIFY2(std::abs(frame.pixelColor(corner).lightnessF() - m_theme->panel().lightnessF()) < 0.15, "Footer actions keep theme chrome");
        if (activation == 0) QVERIFY(tapItem(window(), button));
        else { button->forceActiveFocus(); key(activation == 1 ? Qt::Key_Return : Qt::Key_Space); }
        settle(); QVERIFY(menu->isVisible()); key(Qt::Key_Escape);
    }
    for (const QString label : {"keys", "cmd"}) {
        auto *button = hint(label); QVERIFY(button);
        QVERIFY(tapItem(window(), button)); settle();
        QVERIFY(window()->property("blocked").toBool());
        key(Qt::Key_Escape);
        QVERIFY(!window()->property("blocked").toBool());
    }
    auto *mark = hint("mark"); QVERIFY(mark);
    QVERIFY(tapItem(window(), mark)); settle();
    QCOMPARE(folder->markedCount(), 1);
    QVERIFY(tapItem(window(), mark)); settle();
    QCOMPARE(folder->markedCount(), 0);
    auto *filter = hint("filter"); QVERIFY(filter);
    QVERIFY(tapItem(window(), filter)); settle();
    QVERIFY(window()->property("filtering").toBool());
    auto *keep = hint("keep"); QVERIFY(keep);
    QVERIFY(tapItem(window(), keep)); settle();
    QVERIFY(!window()->property("filtering").toBool());
    key('r'); QVERIFY(!window()->property("prompt").toString().isEmpty());
    auto *cancel = hint("cancel"); QVERIFY(cancel);
    QVERIFY(tapItem(window(), cancel)); settle();
    QVERIFY(window()->property("prompt").toString().isEmpty());
    QTest::qWait(150);
    for (auto *item : itemsNamed("hintAction")) {
        if (!item->isVisible()) continue;
        QVERIFY(item->isEnabled());
        QCOMPARE(item->opacity(), 1.0);
        QVERIFY(QRectF(QPointF(), window()->size()).contains(item->mapRectToScene(item->boundingRect())));
    }
}

void TestStudio::cancelsRowHoldOnMovement()
{
    focusRow("one.txt");
    QTRY_VERIFY(visibleRow("fileRow", cursor()));
    QQuickItem *row = visibleRow("fileRow", cursor());
    static auto *device = QTest::createTouchDevice();
    auto sequence = QTest::touchEvent(window(), device, false);
    // The right side of the row is outside its native file-drag grip.
    QPoint at = row->mapToScene(QPointF(row->width() - 80, row->height() / 2)).toPoint();
    sequence.press(0, at, window()).commit();
    sequence.move(0, at - QPoint(40, 0), window()).commit();
    QTest::qWait(QGuiApplication::styleHints()->mousePressAndHoldInterval() + 150);
    sequence.release(0, at - QPoint(40, 0), window()).commit(); settle();
    QVERIFY(!itemsNamed("fileMenu").first()->isVisible());
    QTest::mousePress(window(), Qt::LeftButton, {}, at);
    QTest::qWait(QGuiApplication::styleHints()->mousePressAndHoldInterval() + 150);
    QVERIFY(!itemsNamed("fileMenu").first()->isVisible());
    QTest::mouseRelease(window(), Qt::LeftButton, {}, at);
}

void TestStudio::opensRowMenusByPen()
{
    for (const QString &mode : {QString("list"), QString("details"), QString("icons")}) {
        tab()->setViewMode(mode); settle(); focusRow("one.txt");
        QTRY_VERIFY(visibleRow("fileRow", cursor()));
        QQuickItem *row = visibleRow("fileRow", cursor());
        openRequests().clear(); verbRuns().clear();
        QVERIFY(tapItem(window(), row, true, true)); settle();
        QVERIFY(itemsNamed("fileMenu").first()->isVisible());
        QVERIFY(openRequests().isEmpty()); QVERIFY(verbRuns().isEmpty());
        key(Qt::Key_Escape);
    }
}

void TestStudio::traversesEveryMenuAction_data()
{
    QTest::addColumn<QString>("name");
    QTest::addColumn<QSize>("size");
    QTest::addColumn<int>("activation");
    for (const QString name : {"fileMenu", "tabMenu", "openWithMenu", "scriptsMenu", "filterMenu",
                              "tagMenu", "viewMenu", "columnMenu", "newTabMenu", "commandPalette"})
        for (const QSize size : {QSize(1200, 800), QSize(420, 300)})
            for (int activation : {Qt::Key_Return, Qt::Key_Space})
                QTest::newRow(qPrintable(QString("%1-%2-%3").arg(name).arg(size.width()).arg(activation)))
                    << name << size << activation;
}

void TestStudio::traversesEveryMenuAction()
{
    QFETCH(QString, name);
    QFETCH(QSize, size);
    QFETCH(int, activation);
    const QSize original = window()->size();
    const auto restore = qScopeGuard([&] { window()->resize(original); });
    window()->resize(size);
    focusRow("one.txt");
    auto *source = itemsNamed(name).first();
    if (name == "openWithMenu") source->setProperty("apps", QVariantList{
        QVariantMap{{"file", "one.desktop"}, {"name", "First viewer"}},
        QVariantMap{{"file", "two.desktop"}, {"name", "Second viewer"}}});
    if (name == "scriptsMenu") source->setProperty("scripts", QVariantList{
        QVariantMap{{"path", "/fake/one"}, {"name", "First script"}},
        QVariantMap{{"path", "/fake/two"}, {"name", "Second script"}}});
    if (name == "columnMenu") source->setProperty("moving", m_workspace->columns());
    window()->setProperty("filterRow", filesRoot()->property("entry"));
    tab()->setCriteria(QVariantMap{{"kind", "file"}});
    QVariantList actions = source->property(name == "commandPalette" ? "commands" : "actions").toList();
    QVERIFY2(!actions.isEmpty(), qPrintable(name));
    for (auto &action : actions) {
        auto entry = action.toMap();
        entry.remove("when");
        action = entry;
    }
    // Exercise real action declarations in a component without command side effects.
    const bool palette = name == "commandPalette";
    QQmlComponent component(m_engine, QUrl(palette ? "qrc:/qml/CommandPalette.qml" : "qrc:/qml/ContextMenu.qml"));
    QScopedPointer<QQuickItem> overlay(qobject_cast<QQuickItem *>(component.create()));
    QVERIFY2(overlay, qPrintable(component.errorString()));
    overlay->setParentItem(window()->contentItem()); overlay->setSize(size); overlay->setZ(200);
    overlay->setProperty(palette ? "commands" : "actions", actions);
    QSignalSpy chosen(overlay.data(), SIGNAL(chosen(QString)));
    for (int target = 0; target < actions.size(); ++target) {
        if (palette) QVERIFY(QMetaObject::invokeMethod(overlay.data(), "open"));
        else QVERIFY(QMetaObject::invokeMethod(overlay.data(), "openAt", Q_ARG(QVariant, 400), Q_ARG(QVariant, 290)));
        settle();
        auto *list = overlay->findChild<QQuickItem *>("overlayList");
        QVERIFY(list);
        for (int step = 0; step < target; ++step) key(Qt::Key_Tab);
        QCOMPARE(list->property("currentIndex").toInt(), target);
        if (target > 0) {
            key(Qt::Key_Backtab, Qt::ShiftModifier);
            QCOMPARE(list->property("currentIndex").toInt(), target - 1);
            key(Qt::Key_Tab);
        }
        auto *current = list->property("currentItem").value<QQuickItem *>();
        QVERIFY(current);
        QTRY_VERIFY(current->mapToItem(list, QPointF()).y() >= -0.5);
        QTRY_VERIFY(current->mapToItem(list, QPointF(0, current->height())).y() <= list->height() + 0.5);
        QVERIFY(overlay->isAncestorOf(window()->activeFocusItem()));
        const QColor fill = current->property("color").value<QColor>();
        QCOMPARE(fill, m_theme->fill(m_theme->accent(), 0.18));
        chosen.clear();
        if (palette && activation == Qt::Key_Space) {
            auto *query = overlay->findChild<QQuickItem *>("paletteQuery");
            QVERIFY(query && query->hasActiveFocus());
            key(Qt::Key_Space);
            QCOMPARE(query->property("text").toString(), QStringLiteral(" "));
            QVERIFY(chosen.isEmpty());
            list->setProperty("currentIndex", target);
            key(Qt::Key_Return);
        } else key(static_cast<Qt::Key>(activation));
        QCOMPARE(chosen.size(), 1);
        QCOMPARE(chosen.first().first().toString(), actions.at(target).toMap().value("id").toString());
        QMetaObject::invokeMethod(overlay.data(), "close");
        qInfo().noquote() << "Inventory PASS" << name << actions.at(target).toMap().value("id").toString();
    }
}

void TestStudio::traversesEveryPermissionControl_data()
{
    QTest::addColumn<QSize>("size");
    QTest::addColumn<bool>("reverse");
    QTest::addColumn<int>("activation");
    for (const QSize size : {QSize(1200, 800), QSize(420, 300)})
        for (bool reverse : {false, true})
            for (int activation : {Qt::Key_Return, Qt::Key_Space})
                QTest::newRow(qPrintable(QString("%1-%2-%3").arg(size.width()).arg(reverse).arg(activation)))
                    << size << reverse << activation;
}

void TestStudio::traversesEveryPermissionControl()
{
    QFETCH(QSize, size); QFETCH(bool, reverse); QFETCH(int, activation);
    const QSize original = window()->size();
    const auto restore = qScopeGuard([&] { window()->resize(original); });
    window()->resize(size); focusRow("one.txt");
    auto *sheet = itemsNamed("modeSheet").first();
    const auto open = [&] {
        return QMetaObject::invokeMethod(sheet, "openFor", Q_ARG(QVariant, QStringList{m_tree.filePath("one.txt")}),
            Q_ARG(QVariant, (QVariantMap{{"permissions", 0644}, {"mine", true}})));
    };
    QVERIFY(open()); settle();
    QList<QQuickItem *> chain;
    for (int bit = 0; bit < 9; ++bit) chain.append(itemNamedWithin(sheet, "modeBit" + QString::number(bit)));
    chain << sheet->findChild<QQuickItem *>("modeApply") << sheet->findChild<QQuickItem *>("modeCancel");
    QVERIFY(!chain.contains(nullptr));
    QTRY_COMPARE(window()->activeFocusItem(), chain.first());
    const auto runs = verbRuns();
    for (int step = 0; step < chain.size(); ++step) {
        const int at = reverse ? (chain.size() - step) % chain.size() : step;
        auto *control = chain.at(at);
        QCOMPARE(window()->activeFocusItem(), control);
        QTRY_VERIFY(hasFocusOutline(window(), control, m_theme->accent()));
        if (at < 9) {
            const int before = sheet->property("value").toInt();
            key(static_cast<Qt::Key>(activation));
            QCOMPARE(sheet->property("value").toInt(), before ^ (1 << (8 - at)));
            key(static_cast<Qt::Key>(activation));
            QCOMPARE(sheet->property("value").toInt(), before);
            QCOMPARE(verbRuns(), runs);
        }
        key(reverse ? Qt::Key_Backtab : Qt::Key_Tab, reverse ? Qt::ShiftModifier : Qt::NoModifier);
    }
    QCOMPARE(window()->activeFocusItem(), chain.first());
    key('l'); QCOMPARE(window()->activeFocusItem(), chain.at(1));
    key('h'); QCOMPARE(window()->activeFocusItem(), chain.first());
    FileManager manager; manager.setWorkspace(m_workspace);
    manager.ShowItemProperties({QUrl::fromLocalFile(m_tree.filePath("one.txt")).toString()}, "");
    settle(); key(Qt::Key_Escape);
    QCOMPARE(window()->activeFocusItem(), chain.first());
    chain.at(9)->forceActiveFocus(); key(static_cast<Qt::Key>(activation));
    QVERIFY(!sheet->isVisible()); QCOMPARE(verbRuns().size(), runs.size() + 1);
    QVERIFY(open()); settle();
    QTRY_COMPARE(window()->activeFocusItem(), chain.first());
    chain.last()->forceActiveFocus(); key(static_cast<Qt::Key>(activation));
    QVERIFY(!sheet->isVisible()); QCOMPARE(verbRuns().size(), runs.size() + 1);
}

void TestStudio::traversesHelpViewport()
{
    const QSize original = window()->size();
    const auto restore = qScopeGuard([&] { window()->resize(original); });
    for (const QSize size : {QSize(1200, 800), QSize(420, 300)}) {
        window()->resize(size); key('?');
        auto *sheet = itemsNamed("helpSheet").first();
        auto *scroll = sheet->findChild<QQuickItem *>("overlayScroll");
        QVERIFY(scroll); QCOMPARE(window()->activeFocusItem(), scroll);
        for (int activation : {Qt::Key_Return, Qt::Key_Space}) {
            key(Qt::Key_Tab); QCOMPARE(window()->activeFocusItem(), scroll);
            key(Qt::Key_Backtab, Qt::ShiftModifier); QCOMPARE(window()->activeFocusItem(), scroll);
            QTRY_VERIFY(hasFocusOutline(window(), scroll, m_theme->accent()));
            key(static_cast<Qt::Key>(activation)); QVERIFY(sheet->isVisible());
        }
        key(Qt::Key_End); QVERIFY(sheet->isVisible());
        key(Qt::Key_Home); QCOMPARE(scroll->property("contentY").toReal(), 0);
        key(Qt::Key_Escape); QVERIFY(!sheet->isVisible());
    }
}

void TestStudio::traversesSidebarActions_data()
{
    QTest::addColumn<QSize>("size");
    QTest::newRow("normal") << QSize(1200, 800);
    QTest::newRow("compact") << QSize(420, 300);
}

void TestStudio::traversesSidebarActions()
{
    QFETCH(QSize, size);
    const QSize original = window()->size();
    const auto restore = qScopeGuard([&] { window()->resize(original); QMetaObject::invokeMethod(window(), "takeFocus"); });
    window()->resize(size);
    focusRow("one.txt");
    if (size.width() < 640) { QMetaObject::invokeMethod(window(), "focusTags"); settle(); }
    auto *details = itemsNamed("detailsMode").first();
    QList<QPair<QString, QString>> controls;
    for (int bit = 0; bit < 9; ++bit) controls.append(QPair<QString, QString>{"modeBit" + QString::number(bit), ""});
    for (auto *chip : itemsNamed("detailsTagChip")) if (chip->isVisible()) controls.append(QPair<QString, QString>{"detailsTagChip", chip->property("modelData").toString()});
    for (const QString list : {"sidebarViews", "sidebarTags"})
        for (int row = 0; row < itemsNamed(list).first()->property("count").toInt(); ++row)
            controls.append(QPair<QString, QString>{list, QString::number(row)});
    controls.append(QPair<QString, QString>{"editTagsButton", ""});
    for (const auto &identity : controls) {
        const auto find = [&]() -> QQuickItem * {
            for (auto *item : itemsNamed(identity.first)) {
                if (!item->isVisible()) continue;
                if (identity.first.startsWith("modeBit") && !details->isAncestorOf(item)) continue;
                if (identity.first == "detailsTagChip" && item->property("modelData").toString() != identity.second) continue;
                return item;
            }
            return nullptr;
        };
        focusRow("one.txt");
        if (size.width() < 640) { QMetaObject::invokeMethod(window(), "focusTags"); settle(); }
        auto *control = find(); QVERIFY(control);
        bool reached = false;
        for (int step = 0; step < 100; ++step) {
            key(Qt::Key_Tab);
            if (control->hasActiveFocus()) { reached = true; break; }
        }
        QVERIFY2(reached, qPrintable(control->objectName()));
        QTRY_VERIFY2(hasFocusOutline(window(), control, m_theme->accent()),
                     qPrintable(identity.first + " " + identity.second));
        key(Qt::Key_Backtab, Qt::ShiftModifier);
        key(Qt::Key_Tab);
        QTRY_VERIFY(control->hasActiveFocus());
        for (int activation : {Qt::Key_Return, Qt::Key_Space}) {
            if (size.width() < 640) { QMetaObject::invokeMethod(window(), identity.first == "sidebarViews" ? "focusViews" : "focusTags"); settle(); }
            control = find(); QVERIFY(control); control->forceActiveFocus();
            if (identity.first == "sidebarViews" || identity.first == "sidebarTags") {
                key(Qt::Key_Home);
                for (int step = 0; step < identity.second.toInt(); ++step) key(Qt::Key_Down);
                QCOMPARE(control->property("currentIndex").toInt(), identity.second.toInt());
            }
            auto *origin = tab();
            const auto runs = verbRuns();
            const int value = details->property("value").toInt();
            key(static_cast<Qt::Key>(activation));
            if (identity.first.startsWith("modeBit")) {
                QCOMPARE(verbRuns().size(), runs.size() + 1);
                QCOMPARE(details->property("value").toInt(), value);
            } else if (identity.first == "detailsTagChip" || identity.first == "editTagsButton") {
                QVERIFY(itemsNamed("tagSheet").first()->isVisible()); key(Qt::Key_Escape);
            } else if (identity.first == "sidebarTags") {
                const int index = control->property("currentIndex").toInt();
                QCOMPARE(tab()->selectedTag(), m_index->tagAt(index));
                tab()->setSelectedTag(""); settle();
            } else {
                QCOMPARE(identity.first, QStringLiteral("sidebarViews"));
                QVERIFY(tab() && tab() != origin);
                m_workspace->closeCurrentTab(); settle();
                QCOMPARE(tab(), origin);
            }
        }
        qInfo().noquote() << "Inventory PASS sidebar" << identity.first << identity.second;
    }
}

void TestStudio::auditsTagEditorControls_data()
{
    QTest::addColumn<QString>("control");
    QTest::addColumn<int>("activation");
    QTest::addColumn<QSize>("size");
    for (const QString control : {"add", "remove", "rename", "source", "replacement", "suggestion0",
                                 "suggestion1", "suggestion2", "suggestion3", "suggestion4", "suggestion5", "apply", "close"})
        for (int activation : {Qt::Key_Return, Qt::Key_Space})
            for (const QSize size : {QSize(1200, 800), QSize(420, 300)})
                QTest::newRow(qPrintable(QString("%1-%2-%3").arg(control).arg(activation).arg(size.width())))
                    << control << activation << size;
}

void TestStudio::auditsTagEditorControls()
{
    QFETCH(QString, control); QFETCH(int, activation); QFETCH(QSize, size);
    const QSize original = window()->size();
    const auto restore = qScopeGuard([&] { window()->resize(original); });
    window()->resize(size); focusRow("one.txt");
    auto *sheet = itemsNamed("tagSheet").first();
    QVERIFY(QMetaObject::invokeMethod(sheet, "openFor", Q_ARG(QVariant, QStringList{m_tree.filePath("one.txt")}),
        Q_ARG(QVariant, "rename"), Q_ARG(QVariant, "work")));
    settle();
    auto *source = itemsNamed("tagSource").first()->findChild<QQuickItem *>("tagInput");
    auto *destination = itemsNamed("tagDestination").first()->findChild<QQuickItem *>("tagInput");
    QVERIFY(source && destination);
    source->setProperty("text", "work"); destination->setProperty("text", "fresh-tag");
    const auto runs = verbRuns();
    if (control.startsWith("suggestion")) {
        const int index = control.mid(10).toInt();
        sheet->setProperty("operation", "add");
        const QStringList words = {"candidate-a", "candidate-b", "candidate-c", "candidate-d", "candidate-e", "candidate-f"};
        sheet->setProperty("vocabulary", words);
        source->setProperty("text", ""); source->forceActiveFocus(); settle();
        auto *choices = itemsNamed("tagSource").first()->findChild<QQuickItem *>("tagSuggestions");
        QVERIFY(choices); QCOMPARE(choices->property("count").toInt(), 6);
        for (int step = 0; step <= index; ++step) key(Qt::Key_Down);
        QCOMPARE(choices->property("currentIndex").toInt(), index);
        auto *current = choices->property("currentItem").value<QQuickItem *>();
        QVERIFY(current); QTRY_COMPARE(current->property("color").value<QColor>(), m_theme->fill(m_theme->accent(), 0.18));
        QTRY_VERIFY(hasFocusOutline(window(), source->parentItem(), m_theme->accent()));
        key(Qt::Key_Tab); QCOMPARE(source->property("text").toString(), words.at(index));
        key(Qt::Key_Backtab, Qt::ShiftModifier);
        QCOMPARE(window()->activeFocusItem(), itemsNamed("tagMode-rename").first());
        key(Qt::Key_Tab); QCOMPARE(window()->activeFocusItem(), source);
        source->setProperty("text", ""); settle();
        for (int step = 0; step <= index; ++step) key(Qt::Key_Down);
        key(Qt::Key_Return); QCOMPARE(source->property("text").toString(), words.at(index));
        source->setProperty("cursorPosition", words.at(index).size());
        key(Qt::Key_Space); QCOMPARE(source->property("text").toString(), words.at(index) + " ");
        QCOMPARE(verbRuns(), runs);
    } else if (control == "source" || control == "replacement") {
        auto *input = control == "source" ? source : destination;
        input->forceActiveFocus(); settle();
        key(Qt::Key_Tab); QCOMPARE(window()->activeFocusItem(), control == "source" ? destination : itemsNamed("tagApply").first());
        key(Qt::Key_Backtab, Qt::ShiftModifier); QCOMPARE(window()->activeFocusItem(), input);
        QTRY_VERIFY(hasFocusOutline(window(), input->parentItem(), m_theme->accent()));
        const QString text = input->property("text").toString();
        input->setProperty("cursorPosition", text.size()); key(Qt::Key_Space);
        QCOMPARE(input->property("text").toString(), text + " "); QCOMPARE(verbRuns(), runs);
        input->setProperty("text", text); key(Qt::Key_Return);
        if (control == "source") QCOMPARE(window()->activeFocusItem(), destination);
        else { QVERIFY(!sheet->isVisible()); QCOMPARE(verbRuns().size(), runs.size() + 1); }
    } else {
        const QString name = control == "apply" ? "tagApply" : control == "close" ? "tagClose" : "tagMode-" + control;
        auto *button = itemsNamed(name).first();
        source->forceActiveFocus();
        bool reached = false;
        for (int step = 0; step < 8; ++step) {
            key(Qt::Key_Tab);
            if (window()->activeFocusItem() == button) { reached = true; break; }
        }
        QVERIFY2(reached, qPrintable(name));
        key(Qt::Key_Backtab, Qt::ShiftModifier); key(Qt::Key_Tab);
        QCOMPARE(window()->activeFocusItem(), button);
        QTRY_VERIFY(hasFocusOutline(window(), button, m_theme->accent()));
        key(static_cast<Qt::Key>(activation));
        if (control == "apply") { QVERIFY(!sheet->isVisible()); QCOMPARE(verbRuns().size(), runs.size() + 1); }
        else if (control == "close") { QVERIFY(!sheet->isVisible()); QCOMPARE(verbRuns(), runs); }
        else { QCOMPARE(sheet->property("operation").toString(), control); QCOMPARE(window()->activeFocusItem(), source); QCOMPARE(verbRuns(), runs); }
    }
    if (sheet->isVisible()) key(Qt::Key_Escape);
}


void TestStudio::auditsPreviewButtons_data()
{
    QTest::addColumn<QString>("control");
    QTest::addColumn<int>("activation");
    QTest::addColumn<QSize>("size");
    for (const QString control : {"Previous", "Next", "Play", "SeekBack", "SeekForward", "Close"})
        for (int activation : {Qt::Key_Return, Qt::Key_Space})
            for (const QSize size : {QSize(1200, 800), QSize(420, 300)})
                QTest::newRow(qPrintable(QString("%1-%2-%3").arg(control).arg(activation).arg(size.width())))
                    << control << activation << size;
}

void TestStudio::auditsPreviewButtons()
{
    ScriptedPlayback playback;
    QVERIFY(playback.isValid());
    QFETCH(QString, control); QFETCH(int, activation); QFETCH(QSize, size);
    const QSize original = window()->size();
    const auto restore = qScopeGuard([&] { window()->resize(original); });
    window()->resize(size); focusRow("one.txt");
    auto *sheet = itemsNamed("previewSheet").first();
    const auto close = qScopeGuard([&] { QMetaObject::invokeMethod(sheet, "close"); });
    QTemporaryDir scratch; QVERIFY(scratch.isValid());
    QFile audio(scratch.filePath("silent.wav")); QVERIFY(audio.open(QIODevice::WriteOnly));
    QCOMPARE(audio.write(silentWave()), qint64(silentWave().size())); audio.close();
    const bool stepping = control == "Previous" || control == "Next";
    if (stepping) key('i');
    else QVERIFY(QMetaObject::invokeMethod(sheet, "openFor", Q_ARG(QVariant, audio.fileName())));
    QTRY_VERIFY(sheet->isVisible());
    auto *preview = qobject_cast<Preview *>(itemsNamed("quickPreview").first()); QVERIFY(preview);
    if (!stepping) { QTRY_COMPARE(preview->kind(), QStringLiteral("audio")); QVERIFY(!preview->playing()); }
    auto *button = itemsNamed("preview" + control).first();
    bool reached = false;
    for (int step = 0; step < 12; ++step) {
        key(Qt::Key_Tab);
        if (button->hasActiveFocus()) { reached = true; break; }
    }
    QVERIFY2(reached, qPrintable(control));
    key(Qt::Key_Backtab, Qt::ShiftModifier); key(Qt::Key_Tab);
    QVERIFY(button->hasActiveFocus());
    QTRY_VERIFY(hasFocusOutline(window(), button, m_theme->accent()));
    QSignalSpy steps(sheet, SIGNAL(stepRequested(int)));
    if (control.startsWith("Seek")) {
        preview->togglePlayback(); QTRY_VERIFY(preview->playing());
        preview->pause(); QTRY_VERIFY(!preview->playing());
    }
    if (control == "SeekBack") { preview->seek(5); QTRY_COMPARE(preview->text(), QStringLiteral("Paused · 0:04 / 0:04")); }
    key(static_cast<Qt::Key>(activation));
    if (stepping) { QCOMPARE(steps.size(), 1); QCOMPARE(steps.first().first().toInt(), control == "Previous" ? -1 : 1); }
    else if (control == "Play") { QTRY_VERIFY(preview->playing()); key(static_cast<Qt::Key>(activation)); QTRY_VERIFY(!preview->playing()); }
    else if (control == "SeekBack") QTRY_COMPARE(preview->text(), QStringLiteral("Paused · 0:00 / 0:04"));
    else if (control == "SeekForward") QTRY_COMPARE(preview->text(), QStringLiteral("Paused · 0:04 / 0:04"));
    else QVERIFY(!sheet->isVisible());
    if (sheet->isVisible()) key(Qt::Key_Escape);
    qInfo().noquote() << "Inventory PASS preview" << control;
}

void TestStudio::auditsInlinePromptButtons_data()
{
    QTest::addColumn<QString>("mode"); QTest::addColumn<bool>("submit");
    QTest::addColumn<int>("activation"); QTest::addColumn<QSize>("size");
    for (const QString mode : {"rename", "folder", "match", "find", "view", "set", "filter", "path"})
        for (bool submit : {false, true})
            for (int activation : {Qt::Key_Return, Qt::Key_Space})
                for (const QSize size : {QSize(1200, 800), QSize(420, 300)})
                    QTest::newRow(qPrintable(QString("%1-%2-%3-%4").arg(mode).arg(submit).arg(activation).arg(size.width())))
                        << mode << submit << activation << size;
}

void TestStudio::auditsInlinePromptButtons()
{
    QFETCH(QString, mode); QFETCH(bool, submit); QFETCH(int, activation); QFETCH(QSize, size);
    const QSize original = window()->size();
    auto *folder = qobject_cast<FolderModel *>(filesRoot()->property("folder").value<QObject *>());
    QVERIFY(folder); folder->clearMarks(); m_index->setNotice("made earlier");
    const auto restore = qScopeGuard([&] { folder->clearMarks(); tab()->setPattern(""); tab()->setRecent(false); window()->resize(original); });
    tab()->setPattern(""); tab()->setRecent(false); settle();
    window()->resize(size); focusRow("one.txt");
    openInlinePrompt(mode);
    settle();
    auto *input = window()->activeFocusItem(); QVERIFY(input && input->inherits("QQuickTextInput"));
    const QMap<QString, QString> drafts = {{"rename", "renamed.txt"}, {"folder", "new-folder"},
        {"match", "*.txt"}, {"find", "*.txt"}, {"view", "keyboard-inventory"},
        {"set", "{stem}-copy{ext}"}, {"filter", "one"}, {"path", m_tree.path()}};
    input->setProperty("text", drafts.value(mode));
    auto *status = itemsNamed("statusBar").first();
    QTRY_VERIFY2(status->property(submit ? "lastHint" : "firstHint").value<QQuickItem *>() != input,
        qPrintable(QString("status width=%1 editing=%2 alarm=%3").arg(status->width()).arg(status->property("editing").toBool()).arg(status->property("alarm").toBool())));
    auto *button = status->property(submit ? "lastHint" : "firstHint").value<QQuickItem *>();
    QVERIFY(button && button != input);
    bool reached = false;
    for (int step = 0; step < 4; ++step) { key(Qt::Key_Tab); if (button->hasActiveFocus()) { reached = true; break; } }
    QVERIFY(reached);
    key(Qt::Key_Backtab, Qt::ShiftModifier); key(Qt::Key_Tab);
    QVERIFY(button->hasActiveFocus());
    QTRY_VERIFY(hasFocusOutline(window(), button, m_theme->accent()));
    const auto runs = verbRuns();
    key(static_cast<Qt::Key>(activation));
    QTRY_VERIFY(!window()->property("blocked").toBool());
    if (!submit) QCOMPARE(verbRuns(), runs);
    qInfo().noquote() << "Inventory PASS prompt" << mode << (submit ? "submit" : "cancel");
}


void TestStudio::auditsContextualFooterControls()
{
    QQmlComponent component(m_engine, QUrl("qrc:/qml/Hints.qml"));
    std::unique_ptr<QObject> object(component.create()); QVERIFY2(object, qPrintable(component.errorString()));
    auto *strip = qobject_cast<QQuickItem *>(object.get()); QVERIFY(strip);
    strip->setParentItem(window()->contentItem()); strip->setZ(200);
    strip->setWidth(2500); strip->setHeight(22);
    const QSize original = window()->size(); window()->resize(2600, 800);
    const auto restore = qScopeGuard([&] { window()->resize(original); });
    QSignalSpy chosen(strip, SIGNAL(requested(QString)));
    for (const QString region : {"list", "tags", "views", "graph"}) {
        QMetaObject::invokeMethod(window(), region == "tags" ? "focusTags" : region == "views" ? "focusViews" : "takeFocus");
        if (region == "graph") m_workspace->addTab(QStringLiteral("graph"));
        strip->setProperty("model", window()->property("hintsForRegion")); settle();
        for (auto *button : itemsNamed("hintAction")) {
            if (!strip->isAncestorOf(button)) continue;
            QVERIFY(button->isEnabled());
            button->forceActiveFocus();
            auto *previous = window()->activeFocusItem();
            key(Qt::Key_Tab); key(Qt::Key_Backtab, Qt::ShiftModifier);
            QCOMPARE(window()->activeFocusItem(), previous);
            QTRY_VERIFY(hasFocusOutline(window(), button, m_theme->accent()));
            for (int activation : {Qt::Key_Return, Qt::Key_Space}) {
                chosen.clear(); key(static_cast<Qt::Key>(activation));
                QCOMPARE(chosen.size(), 1);
                QCOMPARE(chosen.first().first().toString(), button->property("modelData").toMap().value("key").toString());
            }
            qInfo().noquote() << "Inventory PASS footer" << region << button->property("modelData");
        }
        if (region == "graph") m_workspace->closeCurrentTab();
    }
    m_index->setNotice("Read the complete receipt"); settle();
    auto *receipt = itemsNamed("statusReceipt").first();
    for (int activation : {Qt::Key_Return, Qt::Key_Space}) {
        receipt->forceActiveFocus();
        key(Qt::Key_Tab); key(Qt::Key_Backtab, Qt::ShiftModifier);
        QVERIFY(receipt->hasActiveFocus());
        QTRY_VERIFY(hasFocusOutline(window(), receipt, m_theme->accent()));
        key(static_cast<Qt::Key>(activation));
        QTRY_VERIFY(window()->findChild<QObject *>("propertiesDialog"));
        key(Qt::Key_Escape);
    }
}

void TestStudio::openInlinePrompt(const QString &mode)
{
    if (mode == "rename") key('r');
    else if (mode == "folder") key('n');
    else if (mode == "match") key('*');
    else if (mode == "find") key('f');
    else if (mode == "view") QVERIFY(QMetaObject::invokeMethod(window(), "startNaming"));
    else if (mode == "set") {
        key(Qt::Key_Space); key('j'); key(Qt::Key_Space); key('r');
        QCOMPARE(window()->property("prompt").toString(), QStringLiteral("set"));
    } else if (mode == "filter") key('/');
    else if (mode == "path") key('l', Qt::ControlModifier);
    else {
        key(':');
        for (char ch : QByteArray("new-folder")) key(ch);
        key(Qt::Key_Return);
    }
}

void TestStudio::focusPermissionApply()
{
    auto *apply = shownItem(window()->contentItem(), "modeApply");
    QVERIFY(apply);
    for (int step = 0; step < 10 && !apply->hasActiveFocus(); ++step) key(Qt::Key_Tab);
    QVERIFY(apply->hasActiveFocus());
}

void TestStudio::keepsEveryFooterActionReachable_data()
{
    QTest::addColumn<QSize>("size");
    for (const QSize size : {QSize(1180, 720), QSize(420, 300)})
        QTest::newRow(qPrintable(QString::number(size.width()))) << size;
}

void TestStudio::keepsEveryFooterActionReachable()
{
    QFETCH(QSize, size);
    const QSize original = window()->size();
    const auto restore = qScopeGuard([&] {
        if (tab()->kind() == "graph") m_workspace->closeCurrentTab();
        window()->resize(original); QMetaObject::invokeMethod(window(), "takeFocus");
    });
    QQuickWindow footerWindow;
    footerWindow.resize(size); footerWindow.show();
    QQmlComponent component(m_engine, QUrl("qrc:/qml/StatusBar.qml"));
    std::unique_ptr<QObject> object(component.create());
    QVERIFY2(object, qPrintable(component.errorString()));
    auto *bar = qobject_cast<QQuickItem *>(object.get()); QVERIFY(bar);
    bar->setParentItem(footerWindow.contentItem()); bar->setZ(200);
    bar->setProperty("total", 999999); bar->setProperty("marked", 999999);
    bar->setProperty("tag", QString(120, 't')); bar->setProperty("filter", QString(120, 'f'));
    QSignalSpy chosen(bar, SIGNAL(hintRequested(QString)));
    for (const QString region : {"list", "tags", "views", "graph"}) {
        if (region == "graph") m_workspace->addTab(QStringLiteral("graph"));
        QMetaObject::invokeMethod(window(), region == "tags" ? "focusTags" : region == "views" ? "focusViews" : "takeFocus");
        bar->setProperty("hints", window()->property("hintsForRegion"));
        // Resize with the actions already present so their layout must update.
        footerWindow.resize(1180, 720); bar->setWidth(1180); settle();
        footerWindow.resize(size); bar->setWidth(size.width()); settle();
        QTRY_VERIFY(bar->height() >= 26);
        bar->setY(size.height() - bar->height());
        footerWindow.grabWindow();
        QList<QQuickItem *> buttons;
        const auto collect = [&](auto &&self, QQuickItem *parent) -> void {
            for (auto *child : parent->childItems()) {
                if (child->objectName() == "hintAction") buttons.append(child);
                self(self, child);
            }
        };
        collect(collect, bar);
        for (auto *button : buttons) {
            QTRY_VERIFY(QRectF(QPointF(), size).contains(button->mapRectToScene(button->boundingRect())));
            QVERIFY(button->isVisible()); QVERIFY(button->isEnabled());
            QCOMPARE(button->opacity(), 1.0);
            const QString expected = button->property("modelData").toMap().value("key").toString();
            button->forceActiveFocus();
            QTest::keyClick(&footerWindow, Qt::Key_Tab); QTest::keyClick(&footerWindow, Qt::Key_Backtab, Qt::ShiftModifier);
            QTRY_VERIFY2(button->hasActiveFocus(), qPrintable(region + " " + expected + " active=" + (footerWindow.activeFocusItem() ? footerWindow.activeFocusItem()->objectName() : "none")));
            QTRY_VERIFY(hasFocusOutline(&footerWindow, button, m_theme->accent()));
            for (int activation : {Qt::Key_Return, Qt::Key_Space}) {
                chosen.clear(); QTest::keyClick(&footerWindow, static_cast<Qt::Key>(activation));
                QCOMPARE(chosen.size(), 1); QCOMPARE(chosen.first().first().toString(), expected);
            }
            for (bool pen : {false, true}) {
                chosen.clear(); QVERIFY(tapItem(&footerWindow, button, false, pen)); settle();
                QCOMPARE(chosen.size(), 1); QCOMPARE(chosen.first().first().toString(), expected);
            }
        }
        QVERIFY(buttons.size() >= 3);
        if (size.width() == 420) QVERIFY(bar->height() > 26);
    }
}

void TestStudio::preservesTagsWrittenBeforeTheOperationLock()
{
    QTemporaryDir fixture;
    QVERIFY(fixture.isValid());
    const QString file = QDir(fixture.path()).filePath(QStringLiteral("file"));
    QVERIFY(writeLooknfeel(file, QStringLiteral("data")));
    JournalDriver driver;
    VerbRunner actual = setVerbRunner({});
    bool injected = false;
    setVerbRunner([actual, &injected, file](const QStringList &args, VerbReceipt done) {
        if (injected) { actual(args, done); return; }
        injected = true;
        actual({QStringLiteral("tag"), QStringLiteral("add"), QStringLiteral("--tag"),
                QStringLiteral("independent"), QStringLiteral("--"), file},
               [actual, args, done](bool ok, const QString &, const QString &note) {
            if (!ok) { done(false, {}, note); return; }
            actual(args, done);
        });
    });
    driver.run({{QStringLiteral("tag"), QStringLiteral("add"), QStringLiteral("--tag"),
                 QStringLiteral("own"), QStringLiteral("--"), file}});
    QTRY_VERIFY(driver.finished);
    QVERIFY2(driver.ok, qPrintable(driver.notice));
    QVERIFY(driver.journal.available());
    driver.undo();
    QTRY_VERIFY(driver.finished);
    QVERIFY2(driver.ok, qPrintable(driver.notice));
    QString why;
    QStringList tags;
    QVERIFY(readTagsXattr(file, &tags, &why));
    QCOMPARE(tags, QStringList{QStringLiteral("independent")});
    Store store(defaultIndexPath());
    QVERIFY(store.load(&why));
    QCOMPARE(store.get(file).tags, tags);
}

void TestStudio::keepsTextReceiptsForIndexMaintenance()
{
    QTemporaryDir fixture;
    QVERIFY(fixture.isValid());
    const QString file = QDir(fixture.path()).filePath(QStringLiteral("stale"));
    QVERIFY(writeLooknfeel(file, QStringLiteral("data")));
    putIndexRow(file, {QStringLiteral("work")});
    QVERIFY(QFile::remove(file));
    JournalDriver driver;
    m_index->pruneTree(fixture.path());
    QTRY_VERIFY(m_index->notice().contains(QStringLiteral("pruned 1 row")));
    m_index->verifyTree(fixture.path());
    QTRY_VERIFY(m_index->notice().contains(QStringLiteral("is clean")));
}

void TestStudio::resolvesTransferConflictsAndUndoesReplacement_data()
{
    QTest::addColumn<QString>("verb");
    QTest::addColumn<QString>("policy");
    for (const QString &verb : {QStringLiteral("cp"), QStringLiteral("mv")})
        for (const QString &policy : {QStringLiteral("replace"), QStringLiteral("skip"), QStringLiteral("keep"), QStringLiteral("cancel")})
            QTest::newRow(qPrintable(verb + policy)) << verb << policy;
}

void TestStudio::resolvesTransferConflictsAndUndoesReplacement()
{
    QFETCH(QString, verb);
    QFETCH(QString, policy);
    QTemporaryDir fixture;
    const QDir dir(fixture.path());
    const QString source = dir.filePath(QStringLiteral("incoming.txt"));
    const QString target = dir.filePath(QStringLiteral("existing.txt"));
    QVERIFY(writeLooknfeel(source, QStringLiteral("incoming")));
    QVERIFY(writeLooknfeel(target, QStringLiteral("existing")));
    putIndexRow(source, {QStringLiteral("incoming")});
    putIndexRow(target, {QStringLiteral("existing")});
    QString why;
    QVERIFY(writeTagsXattr(source, {QStringLiteral("incoming")}, &why));
    QVERIFY(writeTagsXattr(target, {QStringLiteral("existing")}, &why));
    JournalDriver driver;
    QSignalSpy prompt(&driver.journal, &UndoJournal::conflictRequested);
    driver.journal.run({{verb, QStringLiteral("-T"), QStringLiteral("--"), source, target}}, QStringLiteral("transfer"), driver.receipt(), true);
    QTRY_COMPARE(prompt.size(), 1);
    QCOMPARE(prompt.first().at(1).toString(), target);
    driver.journal.resolveConflict(policy, false);
    QTRY_VERIFY(driver.finished);
    QVERIFY2(driver.ok, qPrintable(driver.notice));
    if (policy == QLatin1String("skip") || policy == QLatin1String("cancel")) {
        QCOMPARE(QFileInfo(source).size(), qint64(8));
        QCOMPARE(QFileInfo(target).size(), qint64(8));
        QVERIFY(!driver.journal.available());
        return;
    }
    const QString landed = policy == QLatin1String("keep") ? dir.filePath(QStringLiteral("existing (1).txt")) : target;
    QFile file(landed);
    QVERIFY(file.open(QIODevice::ReadOnly));
    QCOMPARE(file.readAll(), QByteArray("incoming"));
    file.close();
    Store after(defaultIndexPath());
    QVERIFY2(after.load(&why), qPrintable(why));
    QCOMPARE(after.get(landed).tags, QStringList{QStringLiteral("incoming")});
    driver.undo();
    QTRY_VERIFY(driver.finished);
    QVERIFY2(driver.ok, qPrintable(driver.notice));
    QFile restored(target);
    QVERIFY(restored.open(QIODevice::ReadOnly));
    QCOMPARE(restored.readAll(), QByteArray("existing"));
    QVERIFY(QFileInfo::exists(source));
    QVERIFY2(after.load(&why), qPrintable(why));
    QCOMPARE(after.get(target).tags, QStringList{QStringLiteral("existing")});
    QCOMPARE(after.get(source).tags, QStringList{QStringLiteral("incoming")});
    QStringList restoredTags;
    QVERIFY(readTagsXattr(target, &restoredTags, &why));
    QCOMPARE(restoredTags, QStringList{QStringLiteral("existing")});
    QVERIFY(dir.entryList({QStringLiteral(".omafiles-replaced-*")}, QDir::Hidden | QDir::AllEntries).isEmpty());
}

void TestStudio::traversesConflictControls_data()
{
    QTest::addColumn<QSize>("size");
    QTest::addColumn<int>("activation");
    for (const QSize size : {QSize(1200, 800), QSize(420, 300)})
        for (int activation = 0; activation < 5; ++activation)
            QTest::newRow(qPrintable(QStringLiteral("%1-%2").arg(size.width()).arg(activation))) << size << activation;
}

void TestStudio::traversesConflictControls()
{
    QFETCH(QSize, size);
    QFETCH(int, activation);
    auto *sheet = itemsNamed("conflictSheet").first();
    const QSize old = window()->size();
    const auto restore = qScopeGuard([&] { window()->resize(old); QMetaObject::invokeMethod(sheet, "close"); });
    window()->resize(size);
    const auto open = [&] {
        return QMetaObject::invokeMethod(sheet, "openFor", Q_ARG(QVariant, QString(400, 'a')), Q_ARG(QVariant, QString(400, 'b')));
    };
    QVERIFY(open());
    const QStringList order = {QStringLiteral("conflictSkip"), QStringLiteral("conflictKeep"), QStringLiteral("conflictCancel"), QStringLiteral("conflictAll"), QStringLiteral("conflictReplace")};
    for (const QString &name : order) {
        auto *item = sheet->findChild<QQuickItem *>(name);
        QTRY_VERIFY(item->hasActiveFocus());
        QTRY_VERIFY(item->mapToItem(window()->contentItem(), QPointF()).y() >= 0);
        QTRY_VERIFY(item->mapToItem(window()->contentItem(), QPointF(0, item->height())).y() <= window()->height());
        QTRY_VERIFY(hasFocusOutline(window(), item, m_theme->accent()));
        key(Qt::Key_Tab);
    }
    for (auto it = order.crbegin(); it != order.crend(); ++it) {
        key(Qt::Key_Backtab, Qt::ShiftModifier);
        QTRY_VERIFY(sheet->findChild<QQuickItem *>(*it)->hasActiveFocus());
    }
    key(Qt::Key_PageUp);
    key(Qt::Key_PageDown);
    key(Qt::Key_Home);
    key(Qt::Key_End);
    QVERIFY(sheet->isVisible());
    for (const QString &name : order) {
        QVERIFY(open());
        auto *item = sheet->findChild<QQuickItem *>(name);
        item->forceActiveFocus();
        QTRY_VERIFY(item->mapToItem(window()->contentItem(), QPointF(0, item->height())).y() <= window()->height());
        if (activation < 2) key(activation == 0 ? Qt::Key_Return : Qt::Key_Space);
        else if (activation == 2) QTest::mouseClick(window(), Qt::LeftButton, Qt::NoModifier, item->mapToScene(QPointF(item->width()/2, item->height()/2)).toPoint());
        else QVERIFY(tapItem(window(), item, false, activation == 4));
        if (name == QLatin1String("conflictAll")) {
            QTRY_VERIFY(sheet->property("applyAll").toBool());
            QVERIFY(sheet->isVisible());
            key(Qt::Key_Escape);
        }
        QTRY_VERIFY(!sheet->isVisible());
    }
    QVERIFY(open());
    window()->resize(360, 220);
    auto *focused = sheet->property("focusTarget").value<QQuickItem *>();
    QTRY_VERIFY(focused->hasActiveFocus());
    QTRY_VERIFY(focused->mapToItem(window()->contentItem(), QPointF(0, focused->height())).y() <= window()->height());
    key(Qt::Key_Escape);
    QTRY_VERIFY(!sheet->isVisible());
}

void TestStudio::cancelsAfterCompletedTransfers()
{
    QTemporaryDir fixture;
    QDir dir(fixture.path());
    const QString source = dir.filePath(QStringLiteral("source"));
    const QString free = dir.filePath(QStringLiteral("free"));
    const QString occupied = dir.filePath(QStringLiteral("occupied"));
    const QString never = dir.filePath(QStringLiteral("never"));
    QVERIFY(writeLooknfeel(source, QStringLiteral("incoming")));
    QVERIFY(writeLooknfeel(occupied, QStringLiteral("existing")));
    JournalDriver driver;
    QSignalSpy prompts(&driver.journal, &UndoJournal::conflictRequested);
    driver.journal.run({{QStringLiteral("cp"), QStringLiteral("-T"), QStringLiteral("--"), source, free},
                        {QStringLiteral("cp"), QStringLiteral("-T"), QStringLiteral("--"), source, occupied},
                        {QStringLiteral("cp"), QStringLiteral("-T"), QStringLiteral("--"), source, never}},
                       QStringLiteral("batch"), driver.receipt(), true);
    QTRY_COMPARE(prompts.size(), 1);
    QVERIFY(QFileInfo::exists(free));
    driver.journal.resolveConflict(QStringLiteral("cancel"), false);
    QTRY_VERIFY(driver.finished);
    QVERIFY(!QFileInfo::exists(never));
    driver.undo();
    QTRY_VERIFY(driver.finished);
    QVERIFY2(driver.ok, qPrintable(driver.notice));
    QVERIFY(!QFileInfo::exists(free));
    QVERIFY(QFileInfo::exists(occupied));
}

void TestStudio::scopesConflictDecisionsToOneBatch()
{
    QTemporaryDir fixture;
    QDir dir(fixture.path());
    QVector<QStringList> commands;
    for (int n = 0; n < 2; ++n) {
        const QString source = dir.filePath(QStringLiteral("source%1").arg(n));
        const QString target = dir.filePath(QStringLiteral("target%1").arg(n));
        QVERIFY(writeLooknfeel(source, QStringLiteral("incoming")));
        QVERIFY(writeLooknfeel(target, QStringLiteral("existing")));
        commands.append({QStringLiteral("cp"), QStringLiteral("-T"), QStringLiteral("--"), source, target});
    }
    JournalDriver driver;
    QSignalSpy prompts(&driver.journal, &UndoJournal::conflictRequested);
    const auto connection = connect(&driver.journal, &UndoJournal::conflictRequested, &driver.journal,
        [&] { driver.journal.resolveConflict(QStringLiteral("keep"), true); });
    for (int n = 1; n <= 2; ++n) {
        driver.finished = false;
        driver.journal.run(commands, QStringLiteral("batch"), driver.receipt(), true);
        QTRY_VERIFY(driver.finished);
        QVERIFY2(driver.ok, qPrintable(driver.notice));
        QCOMPARE(prompts.size(), n);
        QVERIFY(QFileInfo::exists(dir.filePath(QStringLiteral("target0 (%1)").arg(n))));
        QVERIFY(QFileInfo::exists(dir.filePath(QStringLiteral("target1 (%1)").arg(n))));
    }
    disconnect(connection);
    driver.undo();
    QTRY_VERIFY(driver.finished);
    QVERIFY2(driver.ok, qPrintable(driver.notice));
    QVERIFY(!QFileInfo::exists(dir.filePath(QStringLiteral("target0 (2)"))));
    QVERIFY(QFileInfo::exists(dir.filePath(QStringLiteral("target0 (1)"))));
    driver.undo();
    QTRY_VERIFY(driver.finished);
    QVERIFY2(driver.ok, qPrintable(driver.notice));
    QVERIFY(!QFileInfo::exists(dir.filePath(QStringLiteral("target0 (1)"))));
}

void TestStudio::refusesUndoOfChangedReplacementBackups()
{
    QTemporaryDir fixture;
    QDir dir(fixture.path());
    const QString source = dir.filePath(QStringLiteral("source"));
    const QString target = dir.filePath(QStringLiteral("target"));
    QVERIFY(writeLooknfeel(source, QStringLiteral("incoming")));
    QVERIFY(writeLooknfeel(target, QStringLiteral("existing")));
    JournalDriver driver;
    driver.run({{QStringLiteral("cp"), QStringLiteral("--force"), QStringLiteral("-T"), QStringLiteral("--"), source, target}});
    QTRY_VERIFY(driver.finished);
    QVERIFY2(driver.ok, qPrintable(driver.notice));
    const QStringList backups = dir.entryList({QStringLiteral(".omafiles-replaced-*")}, QDir::Hidden | QDir::Files);
    QCOMPARE(backups.size(), 1);
    const QString backup = dir.filePath(backups.first());
    putIndexRow(backup, {QStringLiteral("external")});
    driver.undo();
    QTRY_VERIFY(driver.finished);
    QVERIFY(!driver.ok);
    QVERIFY(driver.notice.contains(QStringLiteral("indexed")));
    QVERIFY(QFileInfo::exists(target));
    Store store(defaultIndexPath());
    QString why;
    QVERIFY(store.lockAndLoad(&why));
    store.remove(backup);
    QVERIFY(store.commit(&why));
    QVERIFY(writeLooknfeel(backup, QStringLiteral("changed")));
    driver.undo();
    QTRY_VERIFY(driver.finished);
    QVERIFY(!driver.ok);
    QVERIFY(driver.notice.contains(QStringLiteral("changed")));
    QVERIFY(QFileInfo::exists(target));

}

void TestStudio::restoresReplacedSymbolicLinks()
{
    QTemporaryDir fixture;
    QDir dir(fixture.path());
    const QString source = dir.filePath(QStringLiteral("source"));
    const QString target = dir.filePath(QStringLiteral("target"));
    const QString absent = dir.filePath(QStringLiteral("missing"));
    QVERIFY(writeLooknfeel(source, QStringLiteral("incoming")));
    QVERIFY(QFile::link(absent, target));
    JournalDriver driver;
    driver.run({{QStringLiteral("cp"), QStringLiteral("--force"), QStringLiteral("-T"), QStringLiteral("--"), source, target}});
    QTRY_VERIFY(driver.finished);
    QVERIFY2(driver.ok, qPrintable(driver.notice));
    QVERIFY(!QFileInfo(target).isSymLink());
    driver.undo();
    QTRY_VERIFY(driver.finished);
    QVERIFY2(driver.ok, qPrintable(driver.notice));
    QVERIFY(QFileInfo(target).isSymLink());
    QCOMPARE(QFileInfo(target).symLinkTarget(), absent);
    QVERIFY(!QFileInfo::exists(absent));
}

void TestStudio::cleansUnchangedReplacementBackups()
{
    QTemporaryDir fixture;
    QDir dir(fixture.path());
    const QString source = dir.filePath(QStringLiteral("source"));
    const QString target = dir.filePath(QStringLiteral("target"));
    QVERIFY(writeLooknfeel(source, QStringLiteral("incoming")));
    QVERIFY(writeLooknfeel(target, QStringLiteral("existing")));
    {
        JournalDriver driver;
        driver.run({{QStringLiteral("cp"), QStringLiteral("--force"), QStringLiteral("-T"), QStringLiteral("--"), source, target}});
        QTRY_VERIFY(driver.finished);
        QVERIFY2(driver.ok, qPrintable(driver.notice));
        QCOMPARE(dir.entryList({QStringLiteral(".omafiles-replaced-*")}, QDir::Hidden | QDir::Files).size(), 1);
    }
    QTRY_VERIFY(dir.entryList({QStringLiteral(".omafiles-replaced-*")}, QDir::Hidden | QDir::Files).isEmpty());
    QVERIFY(QFileInfo::exists(source));
    QFile file(target);
    QVERIFY(file.open(QIODevice::ReadOnly));
    QCOMPARE(file.readAll(), QByteArray("incoming"));
}

void TestStudio::mirrorsTabTraversal_data()
{
    QTest::addColumn<QString>("scope");
    QTest::addColumn<QSize>("size");
    QTest::addColumn<bool>("nativeBacktab");
    for (const QString scope : {"list", "tags", "views", "graph", "tag-add", "tag-remove",
                               "tag-rename", "tag-empty", "permissions", "refused", "help",
                               "preview", "properties", "conflict", "rename", "folder",
                               "match", "find", "view", "set", "filter", "path"})
        for (const QSize size : {QSize(1200, 800), QSize(420, 300)})
            for (bool nativeBacktab : {false, true})
                QTest::newRow(qPrintable(scope + QString::number(size.width()) + QString::number(nativeBacktab)))
                    << scope << size << nativeBacktab;
}

void TestStudio::mirrorsTabTraversal()
{
    QFETCH(QString, scope); QFETCH(QSize, size); QFETCH(bool, nativeBacktab);
    const Qt::Key backward = nativeBacktab ? Qt::Key_Backtab : Qt::Key_Tab;
    const QSize original = window()->size();
    const auto restore = qScopeGuard([&] { key(Qt::Key_Escape); window()->resize(original); });
    window()->resize(size); focusRow("one.txt");
    auto *folder = qobject_cast<FolderModel *>(filesRoot()->property("folder").value<QObject *>());
    QVERIFY(folder); folder->clearMarks();
    if (scope.startsWith("tag-")) {
        auto *sheet = itemsNamed("tagSheet").first();
        QVERIFY(QMetaObject::invokeMethod(sheet, "openFor",
            Q_ARG(QVariant, QStringList{m_tree.filePath("one.txt")}),
            Q_ARG(QVariant, scope == "tag-empty" ? "add" : scope.mid(4)), Q_ARG(QVariant, "")));
        sheet->setProperty("vocabulary", QStringList{});
        sheet->setProperty("existing", QStringList{});
        if (scope != "tag-empty") {
            itemsNamed("tagSource").first()->findChild<QQuickItem *>("tagInput")->setProperty("text", "source");
            itemsNamed("tagDestination").first()->findChild<QQuickItem *>("tagInput")->setProperty("text", "destination");
        }
    } else if (scope == "permissions" || scope == "refused") {
        QVERIFY(QMetaObject::invokeMethod(itemsNamed("modeSheet").first(), "openFor",
            Q_ARG(QVariant, QStringList{m_tree.filePath("one.txt")}),
            Q_ARG(QVariant, (QVariantMap{{"permissions", 0644}, {"mine", scope == "permissions"}}))));
    } else if (scope == "help") key('?');
    else if (scope == "preview") {
        key('i');
        auto *preview = qobject_cast<Preview *>(itemsNamed("quickPreview").first());
        QVERIFY(preview); QTRY_VERIFY(!preview->loading());
    } else if (scope == "properties") {
        FileManager manager; manager.setWorkspace(m_workspace);
        manager.ShowItemProperties({QUrl::fromLocalFile(m_tree.filePath("one.txt")).toString()}, "");
    } else if (scope == "conflict") {
        QVERIFY(QMetaObject::invokeMethod(itemsNamed("conflictSheet").first(), "openFor",
            Q_ARG(QVariant, "source"), Q_ARG(QVariant, "destination")));
    } else if (scope == "graph") m_workspace->addTab(QStringLiteral("graph"));
    else if (scope == "tags" || scope == "views") QMetaObject::invokeMethod(window(), scope == "tags" ? "focusTags" : "focusViews");
    else if (scope != "list") openInlinePrompt(scope);
    settle();
    if (scope == "list" || scope == "tags" || scope == "views" || scope == "graph")
        key(Qt::Key_Tab);
    const auto identity = [](QQuickItem *item) {
        if (!item) return QStringLiteral("no focus");
        if (item->objectName() == QLatin1String("hintAction"))
            return QStringLiteral("hint:") + item->property("modelData").toMap().value("key").toString();
        return item->objectName() + QString::number(reinterpret_cast<quintptr>(item), 16);
    };
    QStringList chain;
    const QString first = identity(window()->activeFocusItem());
    do {
        const QString from = identity(window()->activeFocusItem());
        QVERIFY2(!chain.contains(from), qPrintable(scope + " repeats before closing its Tab cycle"));
        chain.append(from);
        QVERIFY(chain.size() < 150);
        key(Qt::Key_Tab);
        const QString to = identity(window()->activeFocusItem());
        key(backward, Qt::ShiftModifier);
        QCOMPARE(identity(window()->activeFocusItem()), from);
        key(Qt::Key_Tab);
        QCOMPARE(identity(window()->activeFocusItem()), to);
    } while (identity(window()->activeFocusItem()) != first);
    for (int step = 0; step < chain.size() * 2; ++step) {
        key(backward, Qt::ShiftModifier);
        QCOMPARE(identity(window()->activeFocusItem()), chain.at((chain.size() - 1 - step % chain.size())));
    }
    qInfo().noquote() << "Symmetric focus cycle" << scope << size << chain.size();
}

void TestStudio::mirrorsMenuTraversal_data()
{
    QTest::addColumn<bool>("palette"); QTest::addColumn<QSize>("size");
    for (bool palette : {false, true})
        for (const QSize size : {QSize(1200, 800), QSize(420, 300)})
            QTest::newRow(qPrintable(QString::number(palette) + QString::number(size.width()))) << palette << size;
}

void TestStudio::mirrorsMenuTraversal()
{
    QFETCH(bool, palette); QFETCH(QSize, size);
    QQmlComponent component(m_engine, QUrl(palette ? "qrc:/qml/CommandPalette.qml" : "qrc:/qml/ContextMenu.qml"));
    QScopedPointer<QQuickItem> overlay(qobject_cast<QQuickItem *>(component.create()));
    QVERIFY2(overlay, qPrintable(component.errorString()));
    overlay->setParentItem(window()->contentItem()); overlay->setSize(size); overlay->setZ(200);
    QVariantList actions;
    for (int row = 0; row < 30; ++row)
        actions.append(QVariantMap{{"id", QString::number(row)}, {"label", "Action " + QString::number(row)}});
    overlay->setProperty(palette ? "commands" : "actions", actions);
    if (palette) QVERIFY(QMetaObject::invokeMethod(overlay.data(), "open"));
    else QVERIFY(QMetaObject::invokeMethod(overlay.data(), "openAt", Q_ARG(QVariant, 400), Q_ARG(QVariant, 290)));
    settle();
    auto *list = overlay->findChild<QQuickItem *>("overlayList"); QVERIFY(list);
    for (bool reverse : {false, true}) {
        for (int step = 0; step < actions.size() * 2; ++step) {
            const int before = list->property("currentIndex").toInt();
            key(reverse ? Qt::Key_Backtab : Qt::Key_Tab, reverse ? Qt::ShiftModifier : Qt::NoModifier);
            const int expected = (before + (reverse ? actions.size() - 1 : 1)) % actions.size();
            QCOMPARE(list->property("currentIndex").toInt(), expected);
            auto *current = list->property("currentItem").value<QQuickItem *>(); QVERIFY(current);
            QTRY_VERIFY(current->mapToItem(list, QPointF()).y() >= -0.5);
            QTRY_VERIFY(current->mapToItem(list, QPointF(0, current->height())).y() <= list->height() + 0.5);
            key(reverse ? Qt::Key_Tab : Qt::Key_Backtab, reverse ? Qt::NoModifier : Qt::ShiftModifier);
            QCOMPARE(list->property("currentIndex").toInt(), before);
            key(reverse ? Qt::Key_Backtab : Qt::Key_Tab, reverse ? Qt::ShiftModifier : Qt::NoModifier);
        }
    }
    QMetaObject::invokeMethod(overlay.data(), "close");
}

void TestStudio::reversesFreshControlGroups_data()
{
    QTest::addColumn<bool>("permissions"); QTest::addColumn<QSize>("size");
    for (bool permissions : {false, true})
        for (const QSize size : {QSize(1200, 800), QSize(420, 300)})
            QTest::newRow(qPrintable(QString::number(permissions) + QString::number(size.width()))) << permissions << size;
}

void TestStudio::reversesFreshControlGroups()
{
    QFETCH(bool, permissions); QFETCH(QSize, size);
    const QSize original = window()->size();
    const auto restore = qScopeGuard([&] { window()->resize(original); QMetaObject::invokeMethod(window(), "takeFocus"); });
    window()->resize(size); focusRow("one.txt");
    if (permissions && size.width() < 640) QMetaObject::invokeMethod(window(), "focusTags");
    settle();
    QQuickItem *group = nullptr;
    if (permissions) group = itemsNamed("detailsMode").first();
    else {
        auto *status = itemsNamed("statusBar").first();
        for (auto *hint : itemsNamed("hintAction"))
            if (hint->isVisible() && status->isAncestorOf(hint)) { group = hint->parentItem()->parentItem(); break; }
    }
    QVERIFY(group);
    const QJSValue controls = group->property("controls").value<QJSValue>();
    QList<QQuickItem *> chain;
    for (int at = 0; at < controls.property("length").toInt(); ++at) {
        auto *control = qobject_cast<QQuickItem *>(controls.property(at).toQObject());
        QVERIFY(control); chain.append(control);
    }
    QVERIFY(chain.size() >= 3);
    // Exercise the Shift-modified Tab representation before any forward traversal.
    chain.last()->forceActiveFocus();
    for (int at = chain.size() - 1; at > 0; --at) {
        key(Qt::Key_Tab, Qt::ShiftModifier);
        QCOMPARE(window()->activeFocusItem(), chain.at(at - 1));
    }
    key(Qt::Key_Tab, Qt::ShiftModifier);
    QVERIFY(!group->isAncestorOf(window()->activeFocusItem()));
}

void TestStudio::browsesAndRestoresTrashByKeyboardAndPointer()
{
    QTemporaryDir fixture;
    QVERIFY(fixture.isValid());
    const QByteArray oldData = qgetenv("XDG_DATA_HOME");
    const QSize oldSize = window()->size();
    qputenv("XDG_DATA_HOME", fixture.path().toUtf8());
    auto cleanup = qScopeGuard([&] {
        if (tab()->kind() == "trash") m_workspace->closeCurrentTab();
        qputenv("XDG_DATA_HOME", oldData);
        window()->resize(oldSize);
    });
    const QDir dir(fixture.path());
    QVERIFY(QDir().mkpath(dir.filePath("Trash/files")));
    QVERIFY(QDir().mkpath(dir.filePath("Trash/info")));
    for (const auto &name : {QStringLiteral("first file.txt"), QStringLiteral("second.txt")}) {
        QVERIFY(writeLooknfeel(dir.filePath("Trash/files/") + name, "payload"));
        QVERIFY(writeLooknfeel(dir.filePath("Trash/info/") + name + ".trashinfo",
            "[Trash Info]\nPath=" + QString::fromLatin1(QUrl::toPercentEncoding(dir.filePath(name), "/"))
            + "\nDeletionDate=2026-10-11T13:14:15\n"));
    }
    key(':');
    for (char ch : QByteArray("open trash")) key(ch);
    key(Qt::Key_Return);
    QTRY_COMPARE(tab()->kind(), QStringLiteral("trash"));
    QTRY_VERIFY(!itemsNamed("trashView").isEmpty());
    auto *view = itemsNamed("trashView").first();
    auto *model = view->property("model").value<TrashModel *>();
    QVERIFY(model);
    QTRY_VERIFY(!model->loading());
    QCOMPARE(model->count(), 2);
    QCOMPARE(model->data(model->index(0), TrashModel::OriginalRole).toString(), dir.filePath("first file.txt"));
    QCOMPARE(model->data(model->index(0), TrashModel::DeletedRole).toString(), QStringLiteral("2026-10-11T13:14:15"));
    window()->resize(420, 300);
    auto *list = itemsNamed("trashList").first();
    list->forceActiveFocus();
    key(Qt::Key_End);
    QCOMPARE(list->property("currentIndex").toInt(), 1);
    key(Qt::Key_Home);
    QCOMPARE(list->property("currentIndex").toInt(), 0);
    key(Qt::Key_Return);
    QTRY_VERIFY(window()->findChild<QObject *>("propertiesDialog"));
    QPointer<QObject> detailDialog = window()->findChild<QObject *>("propertiesDialog");
    QTRY_VERIFY(detailDialog->property("visible").toBool());
    QCOMPARE(detailDialog->property("receipt").toString(), model->details(0));
    key(Qt::Key_Escape);
    QTRY_VERIFY(!detailDialog);
    key(Qt::Key_Space);
    QCOMPARE(model->markedCount(), 1);
    key('m');
    auto *menu = itemsNamed("trashMenu").first();
    QTRY_VERIFY(menu->isVisible());
    key(Qt::Key_Escape);
    QTRY_VERIFY(window()->activeFocusItem() == list || list->isAncestorOf(window()->activeFocusItem()));
    auto *rowItem = itemsNamed("trashRow").first();
    static auto *touch = QTest::createTouchDevice();
    auto sequence = QTest::touchEvent(window(), touch, false);
    const QPoint at = rowItem->mapToScene(QPointF(40, 20)).toPoint();
    sequence.press(0, at, window()).commit();
    QTRY_VERIFY(menu->isVisible());
    sequence.release(0, at, window()).commit();
    key(Qt::Key_Escape);
    const QStringList chain{"trashRestore", "trashEmpty", "trashRefresh", "trashList"};
    for (const auto &name : chain) {
        key(Qt::Key_Tab);
        if (name == "trashList") QVERIFY(window()->activeFocusItem() == list || list->isAncestorOf(window()->activeFocusItem()));
        else QCOMPARE(window()->activeFocusItem()->objectName(), name);
    }
    for (auto it = chain.crbegin() + 1; it != chain.crend(); ++it) {
        key(Qt::Key_Tab, Qt::ShiftModifier);
        QCOMPARE(window()->activeFocusItem()->objectName(), *it);
    }
    key(Qt::Key_Escape);
    key('r');
    QTRY_VERIFY(!model->busy() && !model->loading());
    QCOMPARE(model->count(), 1);
    QVERIFY(QFileInfo::exists(dir.filePath("first file.txt")));
    QVERIFY(!QFileInfo::exists(dir.filePath("Trash/info/first file.txt.trashinfo")));
    auto *empty = itemsNamed("trashEmpty").first();
    QTest::mouseClick(window(), Qt::LeftButton, Qt::NoModifier,
                      empty->mapToScene(QPointF(empty->width() / 2, empty->height() / 2)).toPoint());
    auto *confirm = itemsNamed("trashEmptyMenu").first();
    QTRY_VERIFY(confirm->isVisible());
    key(Qt::Key_Return);
    QVERIFY(QFileInfo::exists(dir.filePath("Trash/files/second.txt")));
    key('e');
    QTRY_VERIFY(confirm->isVisible());
    QVERIFY(writeLooknfeel(dir.filePath("Trash/files/arrived"), "new arrival"));
    QVERIFY(writeLooknfeel(dir.filePath("Trash/info/arrived.trashinfo"),
        "[Trash Info]\nPath=" + dir.filePath("arrived") + "\nDeletionDate=2026-10-11T13:14:15\n"));
    QTRY_COMPARE(model->count(), 2);
    QTRY_VERIFY(!model->loading());
    key(Qt::Key_Down);
    key(Qt::Key_Space);
    QTRY_VERIFY(!model->busy() && !model->loading());
    QCOMPARE(model->count(), 1);
    QVERIFY(QFileInfo::exists(dir.filePath("Trash/files/arrived")));
    QVERIFY(!QFileInfo::exists(dir.filePath("Trash/files/second.txt")));
    QVERIFY(model->message().contains("Permanently deleted 1"));
    key('e');
    QTRY_VERIFY(confirm->isVisible());
    key(Qt::Key_Down);
    key(Qt::Key_Return);
    QTRY_VERIFY(!model->busy() && !model->loading());
    QCOMPARE(model->count(), 0);
    // The watcher must notice a first new item after the Trash has become empty.
    QVERIFY(writeLooknfeel(dir.filePath("Trash/files/third"), "new"));
    QVERIFY(writeLooknfeel(dir.filePath("Trash/info/third.trashinfo"),
        "[Trash Info]\nPath=" + dir.filePath("third") + "\nDeletionDate=2026-10-11T13:14:15\n"));
    QTRY_COMPARE(model->count(), 1);
}

void TestStudio::cancelsStaleTrashScans()
{
    QTemporaryDir fixture;
    QVERIFY(fixture.isValid());
    const QByteArray oldData = qgetenv("XDG_DATA_HOME");
    qputenv("XDG_DATA_HOME", (fixture.path() + "/data").toUtf8());
    const QString mount = fixture.path() + "/mount";
    const QString location = mount + "/.Trash-" + QString::number(::getuid());
    QVERIFY(writeLooknfeel(location + "/files/stale", "payload"));
    QVERIFY(writeLooknfeel(location + "/info/stale.trashinfo",
        "[Trash Info]\nPath=stale\nDeletionDate=2026-10-11T13:14:15\n"));
    QSemaphore entered, release;
    auto releaseWorker = qScopeGuard([&] { release.release(); });
    Trash::setMountProvider([&] {
        entered.release();
        release.acquire();
        return QStringList{mount};
    });
    auto restore = qScopeGuard([&] {
        Trash::setMountProvider([] { return QStringList{}; });
        qputenv("XDG_DATA_HOME", oldData);
    });
    auto model = std::make_unique<TrashModel>();
    QCoreApplication::processEvents();
    QTRY_VERIFY(entered.available() > 0);
    entered.acquire();
    QVERIFY(model->loading());
    Trash::setMountProvider([] { return QStringList{}; });
    model->reload();
    QTRY_VERIFY(!model->loading());
    QCOMPARE(model->count(), 0);
    release.release();
    QThreadPool::globalInstance()->waitForDone();
    QCoreApplication::processEvents();
    QCOMPARE(model->count(), 0);
}

void TestStudio::keepsTrashCursorOnReloadAndRestore()
{
    QTemporaryDir fixture;
    QVERIFY(fixture.isValid());
    const QByteArray oldData = qgetenv("XDG_DATA_HOME");
    const QSize oldSize = window()->size();
    window()->resize(420, 300);
    qputenv("XDG_DATA_HOME", fixture.path().toUtf8());
    auto cleanup = qScopeGuard([&] {
        if (tab()->kind() == "trash") m_workspace->closeCurrentTab();
        qputenv("XDG_DATA_HOME", oldData);
        window()->resize(oldSize);
    });
    const QDir dir(fixture.path());
    auto add = [&](const QString &name) {
        return writeLooknfeel(dir.filePath("Trash/files/") + name, "payload")
            && writeLooknfeel(dir.filePath("Trash/info/") + name + ".trashinfo",
                "[Trash Info]\nPath=" + dir.filePath(name) + "\nDeletionDate=2026-10-11T13:14:15\n");
    };
    for (int i = 0; i < 47; ++i) QVERIFY(add(QStringLiteral("b%1").arg(i, 2, 10, QLatin1Char('0'))));
    key(':');
    for (char ch : QByteArray("open trash")) key(ch);
    key(Qt::Key_Return);
    QTRY_VERIFY(!itemsNamed("trashView").isEmpty());
    auto *view = itemsNamed("trashView").first();
    auto *model = view->property("model").value<TrashModel *>();
    QVERIFY(model);
    QTRY_VERIFY(!model->loading());
    QCOMPARE(model->count(), 47);
    auto *list = itemsNamed("trashList").first();
    QTRY_COMPARE(list->property("currentIndex").toInt(), 0);
    key('j');
    QCOMPARE(list->property("currentIndex").toInt(), 1);
    key('k');
    QCOMPARE(list->property("currentIndex").toInt(), 0);
    key(Qt::Key_Down);
    key(Qt::Key_Up);
    QCOMPARE(list->property("currentIndex").toInt(), 0);
    key(Qt::Key_PageDown);
    QVERIFY(list->property("currentIndex").toInt() > 0);
    key(Qt::Key_PageUp);
    QCOMPARE(list->property("currentIndex").toInt(), 0);
    key('a');
    QCOMPARE(model->markedCount(), 47);
    key('A');
    QCOMPARE(model->markedCount(), 0);
    QVERIFY(writeLooknfeel(dir.filePath("b01"), "occupied"));
    key('j');
    key('r');
    QTRY_VERIFY(!model->busy() && !model->loading());
    QVERIFY(model->message().contains("Restored 0"));
    key(Qt::Key_End);
    QCOMPARE(list->property("currentIndex").toInt(), 46);
    QVERIFY(!window()->grabWindow().isNull());
    QCoreApplication::processEvents(QEventLoop::AllEvents);
    QCOMPARE(list->property("currentIndex").toInt(), 46);
    key('r');
    QTRY_COMPARE(model->count(), 46);
    QTRY_VERIFY(!model->loading() && !model->busy());
    QTRY_COMPARE(list->property("currentIndex").toInt(), 45);
    QVERIFY(QFileInfo::exists(dir.filePath("b46")));
    const QString selected = model->pathAt(45);
    QVERIFY(add("a"));
    QTRY_COMPARE(model->count(), 47);
    QTRY_VERIFY(!model->loading());
    QTRY_COMPARE(model->pathAt(list->property("currentIndex").toInt()), selected);
    key(Qt::Key_F5);
    QTRY_VERIFY(!model->loading());
    QTRY_COMPARE(model->pathAt(list->property("currentIndex").toInt()), selected);
    key('r');
    QTRY_VERIFY(QFileInfo::exists(dir.filePath("b45")));
    QTRY_COMPARE(model->count(), 46);
    QTRY_VERIFY(!model->loading() && !model->busy());
    QTRY_COMPARE(list->property("currentIndex").toInt(), 45);
}

void TestStudio::opensFreshOverlaysOnTheirFirstControl_data()
{
    QTest::addColumn<QString>("component");
    QTest::addColumn<QSize>("size");
    for (const QString component : {"PreviewSheet", "HelpSheet", "ContextMenu", "CommandPalette",
                                    "ModeSheet", "TagSheet", "ConflictSheet"})
        for (const QSize size : {QSize(1200, 800), QSize(420, 300)})
            QTest::newRow(qPrintable(component + QString::number(size.width()))) << component << size;
}

void TestStudio::opensFreshOverlaysOnTheirFirstControl()
{
    QFETCH(QString, component);
    QFETCH(QSize, size);
    const QSize original = window()->size();
    const auto restore = qScopeGuard([&] { window()->resize(original); });
    window()->resize(size);
    QQmlComponent factory(m_engine, QUrl("qrc:/qml/" + component + ".qml"));
    QScopedPointer<QQuickItem> overlay(qobject_cast<QQuickItem *>(factory.create()));
    QVERIFY2(overlay, qPrintable(factory.errorString()));
    overlay->setParentItem(window()->contentItem());
    overlay->setSize(size);
    overlay->setZ(200);
    if (component == "PreviewSheet")
        QVERIFY(QMetaObject::invokeMethod(overlay.data(), "openFor", Q_ARG(QVariant, m_tree.filePath("one.txt"))));
    else if (component == "ContextMenu") {
        overlay->setProperty("actions", QVariantList{QVariantMap{{"id", "first"}, {"label", "First"}},
                                                     QVariantMap{{"id", "second"}, {"label", "Second"}}});
        QVERIFY(QMetaObject::invokeMethod(overlay.data(), "openAt", Q_ARG(QVariant, 20), Q_ARG(QVariant, 20)));
    } else if (component == "ModeSheet")
        QVERIFY(QMetaObject::invokeMethod(overlay.data(), "openFor", Q_ARG(QVariant, QStringList{m_tree.filePath("one.txt")}),
            Q_ARG(QVariant, (QVariantMap{{"permissions", 0644}, {"mine", true}}))));
    else if (component == "TagSheet")
        QVERIFY(QMetaObject::invokeMethod(overlay.data(), "openFor", Q_ARG(QVariant, QStringList{m_tree.filePath("one.txt")}),
            Q_ARG(QVariant, "add"), Q_ARG(QVariant, "")));
    else if (component == "ConflictSheet")
        QVERIFY(QMetaObject::invokeMethod(overlay.data(), "openFor", Q_ARG(QVariant, "source"), Q_ARG(QVariant, "target")));
    else QVERIFY(QMetaObject::invokeMethod(overlay.data(), "open"));
    const auto target = [&] { return overlay->property("focusTarget").value<QQuickItem *>(); };
    QTRY_VERIFY(target());
    QTRY_VERIFY(target()->hasActiveFocus());
    auto *first = window()->activeFocusItem();
    QVERIFY(first);
    QVERIFY2(first->activeFocusOnTab() || first->inherits("QQuickTextInput") || component == "ContextMenu", qPrintable(component));
    if (component == "ContextMenu") {
        auto *list = overlay->findChild<QQuickItem *>("overlayList");
        QVERIFY(list);
        QCOMPARE(list->property("currentIndex").toInt(), 0);
        QVERIFY(first == list || list->isAncestorOf(first));
    }
    for (const auto backward : {Qt::Key_Backtab, Qt::Key_Tab}) {
        key(backward, Qt::ShiftModifier);
        key(Qt::Key_Tab);
        QCOMPARE(window()->activeFocusItem(), first);
        key(Qt::Key_Tab);
        key(backward, Qt::ShiftModifier);
        QCOMPARE(window()->activeFocusItem(), first);
    }
    if (component == "PreviewSheet") {
        auto *preview = qobject_cast<Preview *>(overlay->findChild<QObject *>("quickPreview"));
        QVERIFY(preview);
        QTRY_VERIFY(!preview->loading());
        QCOMPARE(window()->activeFocusItem(), first);
    }
    QVERIFY(QMetaObject::invokeMethod(overlay.data(), "close"));
}

void TestStudio::restoresKeyRoutingAfterTabRoundTrips_data()
{
    QTest::addColumn<QSize>("size");
    QTest::addColumn<bool>("nativeBacktab");
    for (const QSize size : {QSize(1100, 800), QSize(1100, 300), QSize(420, 300)})
        for (bool nativeBacktab : {false, true})
            QTest::newRow(qPrintable(QString::number(size.width()) + "x" + QString::number(size.height())
                + QString::number(nativeBacktab))) << size << nativeBacktab;
}

void TestStudio::restoresKeyRoutingAfterTabRoundTrips()
{
    QFETCH(QSize, size);
    QFETCH(bool, nativeBacktab);
    const QSize original = window()->size();
    const auto restore = qScopeGuard([&] { key(Qt::Key_Escape); window()->resize(original); });
    window()->resize(size);
    focusRow("one.txt");
    auto *tags = window()->property("activeTagsList").value<QQuickItem *>();
    auto *views = window()->property("activeViewsList").value<QQuickItem *>();
    QVERIFY(tags && views);
    QVERIFY(tags->property("count").toInt() >= 2);
    QVERIFY(views->property("count").toInt() >= 2);
    const int fileRow = cursor();
    key('j'); QVERIFY(cursor() != fileRow);
    key('k'); QCOMPARE(cursor(), fileRow);
    auto *origin = window()->activeFocusItem();
    const auto hints = [&] { return window()->property("hintsForRegion").toList(); };
    const QVariantList initialHints = hints();
    QVERIFY(!initialHints.isEmpty());
    for (int step = 0; step < 33; ++step) key(Qt::Key_Tab);
    for (int step = 0; step < 33; ++step)
        key(nativeBacktab ? Qt::Key_Backtab : Qt::Key_Tab, Qt::ShiftModifier);
    QCOMPARE(window()->activeFocusItem(), origin);
    QCOMPARE(hints(), initialHints);
    const int tagRow = tags->property("currentIndex").toInt();
    const int viewRow = views->property("currentIndex").toInt();
    key('j'); QVERIFY(cursor() != fileRow);
    QCOMPARE(tags->property("currentIndex").toInt(), tagRow);
    QCOMPARE(views->property("currentIndex").toInt(), viewRow);
    key('k'); QCOMPARE(cursor(), fileRow);
    auto *status = itemsNamed("statusBar").first();
    QQuickItem *footer = nullptr;
    for (auto *item : itemsNamed("hintAction"))
        if (status->isAncestorOf(item) && item->property("modelData").toMap().value("key") == "?")
            footer = item;
    QVERIFY(footer);
    footer->forceActiveFocus();
    key('?'); key(Qt::Key_Escape);
    QTRY_COMPARE(window()->activeFocusItem(), footer);
    QMetaObject::invokeMethod(window(), "takeFocus");
    for (const QString region : {"tags", "views"}) {
        key(region == "tags" ? 't' : 'v');
        auto *list = region == "tags" ? tags : views;
        QVERIFY(list->hasActiveFocus());
        key(Qt::Key_Home);
        key('j'); QCOMPARE(list->property("currentIndex").toInt(), 1);
        QCOMPARE(cursor(), fileRow);
        key('?'); key(Qt::Key_Escape);
        QVERIFY(list->hasActiveFocus());
        key('k'); QCOMPARE(list->property("currentIndex").toInt(), 0);
        key(Qt::Key_D, Qt::ControlModifier);
        QVERIFY(list->property("currentIndex").toInt() > 0);
        QCOMPARE(cursor(), fileRow);
        key(Qt::Key_U, Qt::ControlModifier);
        QCOMPARE(list->property("currentIndex").toInt(), 0);
        key(Qt::Key_Escape);
        QCOMPARE(window()->activeFocusItem(), origin);
        QCOMPARE(hints(), initialHints);
    }
}

void TestStudio::preservesHintTargetsOnPointerActivation_data()
{
    QTest::addColumn<QSize>("size");
    QTest::addColumn<bool>("touch");
    for (const QSize size : {QSize(1100, 800), QSize(420, 300)})
        for (bool touch : {false, true})
            QTest::newRow(qPrintable(QString::number(size.width()) + QString::number(touch))) << size << touch;
}

void TestStudio::preservesHintTargetsOnPointerActivation()
{
    QFETCH(QSize, size); QFETCH(bool, touch);
    const QSize original = window()->size();
    const auto restore = qScopeGuard([&] {
        tab()->setSelectedTag(""); key(Qt::Key_Escape); window()->resize(original);
    });
    const auto activate = [&](QQuickItem *item, qreal relativeX = 0.5) {
        QVERIFY(!window()->grabWindow().isNull());
        const QPoint point = item->mapToScene(QPointF(item->width() * relativeX, item->height() / 2)).toPoint();
        if (touch) {
            static auto *device = QTest::createTouchDevice();
            auto sequence = QTest::touchEvent(window(), device, false);
            sequence.press(0, point, window()).commit();
            sequence.release(0, point, window()).commit();
        } else QTest::mouseClick(window(), Qt::LeftButton, Qt::NoModifier, point);
        settle();
    };
    window()->resize(size); focusRow("one.txt");
    key('t');
    auto *tags = window()->property("activeTagsList").value<QQuickItem *>();
    QVERIFY(tags);
    auto *heading = itemsNamed("tagsHeading").first();
    auto *scroll = itemsNamed("dockScroll").first();
    QVERIFY(QMetaObject::invokeMethod(scroll, "revealItem", Q_ARG(QVariant, QVariant::fromValue(heading))));
    QTRY_VERIFY(QRectF(QPointF(), window()->size()).contains(
        heading->mapToScene(QPointF(heading->width() * 0.1, heading->height() / 2))));
    activate(heading, 0.1);
    QVERIFY(tags->hasActiveFocus());
    key(Qt::Key_Home);
    const QString chosenTag = m_index->tagAt(0);
    QVERIFY(!chosenTag.isEmpty());
    auto *status = itemsNamed("statusBar").first();
    QQuickItem *hint = nullptr;
    for (auto *item : itemsNamed("hintAction"))
        if (status->isAncestorOf(item) && item->property("modelData").toMap().value("key") == "Enter")
            hint = item;
    QVERIFY(hint);
    QCOMPARE(hint->property("modelData").toMap().value("label").toString(), QStringLiteral("toggle"));
    QSignalSpy requested(status, SIGNAL(hintRequested(QString)));
    activate(hint);
    QTRY_COMPARE(requested.size(), 1);
    QCOMPARE(requested.first().first().toString(), QStringLiteral("Enter"));
    QTRY_COMPARE(tab()->selectedTag(), chosenTag);
}

namespace {
struct ArchiveFixtureEntry {
    ArchiveFixtureEntry(QByteArray name, int kind = AE_IFREG, QByteArray target = {}, QByteArray data = "archive data", QByteArray tags = {}, int mode = 0644)
        : name(name), kind(kind), target(target), data(data), tags(tags), mode(mode) {}
    QByteArray name;
    int kind = AE_IFREG;
    QByteArray target;
    QByteArray data = "archive data";
    QByteArray tags;
    int mode = 0644;
};
bool writeArchiveFixture(const QString &path, const QVector<ArchiveFixtureEntry> &entries, bool zip = false)
{
    archive *writer = archive_write_new();
    const auto release = qScopeGuard([writer] { archive_write_free(writer); });
    if ((zip ? archive_write_set_format_zip(writer) : archive_write_set_format_pax_restricted(writer)) != ARCHIVE_OK
        || archive_write_open_filename(writer, QFile::encodeName(path).constData()) != ARCHIVE_OK) return false;
    for (const auto &item : entries) {
        archive_entry *entry = archive_entry_new();
        const auto free = qScopeGuard([entry] { archive_entry_free(entry); });
        archive_entry_set_pathname(entry, item.name.constData());
        archive_entry_set_filetype(entry, item.kind);
        archive_entry_set_perm(entry, item.mode);
        if (!item.target.isEmpty()) {
            if (item.kind == AE_IFLNK) archive_entry_set_symlink(entry, item.target.constData());
            else archive_entry_set_hardlink(entry, item.target.constData());
        }
        const bool bytes = item.kind == AE_IFREG && item.target.isEmpty();
        archive_entry_set_size(entry, bytes ? item.data.size() : 0);
        if (!item.tags.isEmpty()) archive_entry_xattr_add_entry(entry, "user.omafiles.tags", item.tags.constData(), item.tags.size());
        if (archive_write_header(writer, entry) != ARCHIVE_OK) return false;
        if (bytes && archive_write_data(writer, item.data.constData(), item.data.size()) != item.data.size()) return false;
        if (archive_write_finish_entry(writer) != ARCHIVE_OK) return false;
    }
    return archive_write_close(writer) == ARCHIVE_OK;
}
QByteArray archiveTestBytes(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}
}

void TestStudio::roundTripsArchiveFormats_data()
{
    QTest::addColumn<QString>("format");
    for (const QString format : {"zip", "tar.gz", "tar.zst"}) QTest::newRow(qPrintable(format)) << format;
}
void TestStudio::roundTripsArchiveFormats()
{
    QFETCH(QString, format);
    JournalDriver tools;
    QTemporaryDir fixture;
    QVERIFY(fixture.isValid());
    const QDir dir(fixture.path());
    QVERIFY(dir.mkpath("bundle/empty"));
    QVERIFY(dir.mkdir("out"));
    const QString child = dir.filePath("bundle/child.txt");
    QVERIFY(writeLooknfeel(child, "nested payload"));
    QVERIFY(::symlink("child.txt", QFile::encodeName(dir.filePath("bundle/link")).constData()) == 0);
    QString why;
    QVERIFY(writeTagsXattr(child, {"portable"}, &why));
    const QString cwd = QDir::currentPath();
    const QByteArray processState = archiveTestBytes("/proc/self/status");
    const QByteArray mask = processState.mid(processState.indexOf("Umask:"), 12);
    ArchiveJob compression;
    QSignalSpy ready(&compression, &ArchiveJob::ready), failed(&compression, &ArchiveJob::finished);
    bool eventLoopTurn = false;
    QTimer::singleShot(0, &compression, [&] { eventLoopTurn = true; });
    compression.compress({dir.filePath("bundle")}, dir.filePath("bundle." + format), format);
    QVERIFY(compression.busy());
    QTRY_VERIFY(!ready.isEmpty() || !failed.isEmpty());
    QVERIFY2(!ready.isEmpty(), failed.isEmpty() ? "no receipt" : qPrintable(failed.first()[1].toString()));
    QVERIFY(eventLoopTurn);
    QVERIFY(compression.processed() >= 14);
    QCOMPARE(QDir::currentPath(), cwd);
    const QByteArray after = archiveTestBytes("/proc/self/status");
    QCOMPARE(after.mid(after.indexOf("Umask:"), 12), mask);
    const QString archivePath = ready.first()[0].toStringList().first();
    const QByteArray magic = format == "zip" ? QByteArray::fromHex("504b0304")
        : format == "tar.gz" ? QByteArray::fromHex("1f8b") : QByteArray::fromHex("28b52ffd");
    QVERIFY(archiveTestBytes(archivePath).startsWith(magic));
    ArchiveJob extraction;
    QSignalSpy extracted(&extraction, &ArchiveJob::ready), error(&extraction, &ArchiveJob::finished);
    extraction.extract(archivePath, dir.filePath("out"));
    QTRY_VERIFY(!extracted.isEmpty() || !error.isEmpty());
    QVERIFY2(!extracted.isEmpty(), error.isEmpty() ? "no receipt" : qPrintable(error.first()[1].toString()));
    const QString tree = extracted.first()[0].toStringList().first();
    QCOMPARE(archiveTestBytes(tree + "/child.txt"), QByteArray("nested payload"));
    QVERIFY(QFileInfo(tree + "/empty").isDir());
    QVERIFY(QFileInfo(tree + "/link").isSymLink());
    QCOMPARE(linkTarget(QFile::encodeName(tree + "/link")), QByteArray("child.txt"));
    if (format != "zip") {
        QStringList tags;
        QVERIFY(readTagsXattr(tree + "/child.txt", &tags, &why));
        QCOMPARE(tags, QStringList{"portable"});
    }
    extraction.complete(true, "done"); compression.complete(true, "done");
    QTRY_VERIFY(QDir(dir.filePath("out")).entryList({".omafiles-archive-*"}, QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot).isEmpty());
}

void TestStudio::refusesUnsafeArchiveEntries_data()
{
    QTest::addColumn<QString>("attack");
    for (const QString attack : {"parent-tar", "parent-zip", "absolute", "absolute-symlink", "escaping-symlink", "symlink-child", "absolute-hardlink", "hardlink-symlink", "duplicate", "fifo", "truncated"})
        QTest::newRow(qPrintable(attack)) << attack;
}
void TestStudio::refusesUnsafeArchiveEntries()
{
    QFETCH(QString, attack);
    JournalDriver tools;
    QTemporaryDir fixture;
    QVERIFY(fixture.isValid());
    const QDir dir(fixture.path());
    QVERIFY(dir.mkdir("out"));
    const QString outside = dir.filePath("outside");
    QVERIFY(writeLooknfeel(outside, "protected"));
    QVector<ArchiveFixtureEntry> entries;
    if (attack.startsWith("parent")) entries = {{"../outside"}};
    else if (attack == "absolute") entries = {{QFile::encodeName(outside)}};
    else if (attack == "absolute-symlink") entries = {{"link", AE_IFLNK, QFile::encodeName(outside)}};
    else if (attack == "escaping-symlink") entries = {{"link", AE_IFLNK, "../outside"}};
    else if (attack == "symlink-child") entries = {{"link", AE_IFLNK, "inside"}, {"link/child"}};
    else if (attack == "absolute-hardlink") entries = {{"hard", AE_IFREG, QFile::encodeName(outside)}};
    else if (attack == "hardlink-symlink") entries = {{"inside"}, {"link", AE_IFLNK, "inside"}, {"hard", AE_IFREG, "link"}};
    else if (attack == "duplicate") entries = {{"same"}, {"same"}};
    else if (attack == "fifo") entries = {{"pipe", AE_IFIFO}};
    else entries = {{"large", AE_IFREG, {}, QByteArray(65536, 'x')}};
    const QString archivePath = dir.filePath("attack.archive");
    QVERIFY(writeArchiveFixture(archivePath, entries, attack == "parent-zip"));
    if (attack == "truncated") { QFile file(archivePath); QVERIFY(file.open(QIODevice::ReadWrite)); QVERIFY(file.resize(1024)); }
    ArchiveJob job;
    QSignalSpy ready(&job, &ArchiveJob::ready), finished(&job, &ArchiveJob::finished);
    job.extract(archivePath, dir.filePath("out"));
    QTRY_VERIFY(!finished.isEmpty() || !ready.isEmpty());
    QVERIFY2(ready.isEmpty(), qPrintable(attack));
    QVERIFY(!finished.first()[0].toBool());
    QVERIFY(!finished.first()[1].toString().isEmpty());
    QCOMPARE(archiveTestBytes(outside), QByteArray("protected"));
    QTRY_VERIFY(QDir(dir.filePath("out")).entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot).isEmpty());
}

void TestStudio::extractsContainedLinksAndAccessiblePermissions()
{
    JournalDriver tools;
    QTemporaryDir fixture;
    const QDir dir(fixture.path());
    QVERIFY(dir.mkdir("out"));
    const QString archivePath = dir.filePath("links.tar");
    QVERIFY(writeArchiveFixture(archivePath, {{"good.txt", AE_IFREG, {}, "payload", {}, 0000},
        {"hard", AE_IFREG, "good.txt"}, {"symbolic", AE_IFLNK, "good.txt"},
        {"folder", AE_IFDIR, {}, {}, {}, 0000}, {"folder/child"}}));
    ArchiveJob job;
    QSignalSpy ready(&job, &ArchiveJob::ready), finished(&job, &ArchiveJob::finished);
    job.extract(archivePath, dir.filePath("out"));
    QTRY_VERIFY(!ready.isEmpty() || !finished.isEmpty());
    QVERIFY2(!ready.isEmpty(), finished.isEmpty() ? "No archive receipt" : qPrintable(finished.first()[1].toString()));
    const QString stage = QFileInfo(ready.first()[0].toStringList().first()).absolutePath();
    QCOMPARE(archiveTestBytes(stage + "/good.txt"), QByteArray("payload"));
    QCOMPARE(archiveTestBytes(stage + "/hard"), QByteArray("payload"));
    struct stat original {}, linked {};
    QVERIFY(::stat(QFile::encodeName(stage + "/good.txt").constData(), &original) == 0);
    QVERIFY(::stat(QFile::encodeName(stage + "/hard").constData(), &linked) == 0);
    QCOMPARE(original.st_ino, linked.st_ino);
    QCOMPARE(linkTarget(QFile::encodeName(stage + "/symbolic")), QByteArray("good.txt"));
    QCOMPARE(archiveTestBytes(stage + "/folder/child"), QByteArray("archive data"));
    QVERIFY((QFileInfo(stage + "/folder").permissions() & (QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner))
        == (QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
    job.complete(true, "done");
    QTRY_VERIFY(!QFileInfo::exists(stage));
}

void TestStudio::cancelsArchivePreparationWithoutPublishing()
{
    JournalDriver tools;
    QTemporaryDir fixture;
    QVERIFY(fixture.isValid());
    const QDir dir(fixture.path());
    QVERIFY(writeLooknfeel(dir.filePath("source"), "payload"));
    ArchiveJob job;
    QSignalSpy ready(&job, &ArchiveJob::ready), finished(&job, &ArchiveJob::finished);
    job.compress({dir.filePath("source")}, dir.filePath("archive.zip"), "zip");
    QVERIFY(job.busy());
    job.cancel();
    QTRY_COMPARE(finished.size(), 1);
    QVERIFY(!finished.first()[0].toBool());
    QVERIFY(finished.first()[1].toString().contains("canceled", Qt::CaseInsensitive));
    QVERIFY(ready.isEmpty());
    QVERIFY(!QFileInfo::exists(dir.filePath("archive.zip")));
    QTRY_VERIFY(dir.entryList({".omafiles-archive-*"}, QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot).isEmpty());
}

void TestStudio::publishesArchivesWithTagsAndUndo()
{
    QTemporaryDir fixture;
    QVERIFY(fixture.isValid());
    const QDir dir(fixture.path());
    QVERIFY(dir.mkdir("out"));
    const QString source = dir.filePath("source.txt"), archivePath = dir.filePath("source.tar.gz");
    QVERIFY(writeLooknfeel(source, "payload"));
    QString why;
    QVERIFY(writeTagsXattr(source, {"portable"}, &why));
    putIndexRow(source, {"portable"});
    const QString second = dir.filePath("second.txt");
    QVERIFY(writeLooknfeel(second, "another"));
    QVERIFY(writeTagsXattr(second, {"second"}, &why));
    putIndexRow(second, {"second"}); m_index->reload();
    JournalDriver tools;
    QSignalSpy finished(m_index->archive(), &ArchiveJob::finished);
    m_index->compress({source, second}, archivePath, "tar.gz");
    QTRY_COMPARE(finished.size(), 1);
    QVERIFY2(finished.first()[0].toBool(), qPrintable(finished.first()[1].toString()));
    QCOMPARE(m_index->notice(), finished.first()[1].toString());
    Store store(defaultIndexPath()); QVERIFY(store.load(&why));
    QCOMPARE(store.get(archivePath).tags, (QStringList{"portable", "second"}));
    finished.clear();
    m_index->extract(archivePath, dir.filePath("out"));
    QTRY_COMPARE(finished.size(), 1);
    QVERIFY2(finished.first()[0].toBool(), qPrintable(finished.first()[1].toString()));
    QCOMPARE(m_index->notice(), finished.first()[1].toString());
    const QString output = dir.filePath("out/source.txt");
    QCOMPARE(archiveTestBytes(output), QByteArray("payload"));
    QVERIFY(store.load(&why)); QCOMPARE(store.get(output).tags, QStringList{"portable"});
    QCOMPARE(store.get(dir.filePath("out/second.txt")).tags, QStringList{"second"});
    for (const Record &row : store.recordSnapshot()) QVERIFY(!row.path.contains("/.omafiles-archive-"));
    m_index->undo(); QTRY_VERIFY(!m_index->operationBusy());
    QVERIFY(!QFileInfo::exists(output)); QVERIFY(QFileInfo::exists(archivePath));
    m_index->undo(); QTRY_VERIFY(!m_index->operationBusy());
    QVERIFY(!QFileInfo::exists(archivePath)); QCOMPARE(archiveTestBytes(source), QByteArray("payload"));
}

void TestStudio::undoesInstalledArchiveTrees()
{
    QTemporaryDir fixture;
    QVERIFY(fixture.isValid());
    const QDir dir(fixture.path());
    QVERIFY(dir.mkpath("stage/tree/empty")); QVERIFY(dir.mkdir("out"));
    const QString staged = dir.filePath("stage/tree");
    QVERIFY(writeLooknfeel(staged + "/child", "payload"));
    QVERIFY(::symlink("child", QFile::encodeName(staged + "/link").constData()) == 0);
    JournalDriver driver;
    driver.journal.install({staged}, dir.filePath("out"), "extract", driver.receipt());
    QTRY_VERIFY(driver.finished); QVERIFY2(driver.ok, qPrintable(driver.notice));
    QVERIFY(QDir(dir.filePath("stage")).removeRecursively());
    const QString target = dir.filePath("out/tree");
    QVERIFY(writeLooknfeel(target + "/child", "changed"));
    driver.undo(); QTRY_VERIFY(driver.finished);
    QVERIFY(!driver.ok); QVERIFY(driver.notice.contains("changed"));
    QCOMPARE(archiveTestBytes(target + "/child"), QByteArray("changed"));
    // A fresh installation has an unchanged tree and can be removed without its staging parent.
    QVERIFY(dir.mkpath("stage/fresh/empty"));
    QVERIFY(writeLooknfeel(dir.filePath("stage/fresh/child"), "payload"));
    driver.finished = false;
    driver.journal.install({dir.filePath("stage/fresh")}, dir.filePath("out"), "extract", driver.receipt());
    QTRY_VERIFY(driver.finished); QVERIFY2(driver.ok, qPrintable(driver.notice));
    QVERIFY(QDir(dir.filePath("stage")).removeRecursively());
    driver.undo(); QTRY_VERIFY(driver.finished); QVERIFY2(driver.ok, qPrintable(driver.notice));
    QVERIFY(!QFileInfo::exists(dir.filePath("out/fresh")));
}

void TestStudio::resolvesArchiveConflicts_data()
{
    QTest::addColumn<QString>("policy");
    for (const QString policy : {"replace", "skip", "keep", "cancel"}) QTest::newRow(qPrintable(policy)) << policy;
}
void TestStudio::resolvesArchiveConflicts()
{
    QFETCH(QString, policy);
    QTemporaryDir fixture;
    const QDir dir(fixture.path());
    QVERIFY(dir.mkpath("stage")); QVERIFY(dir.mkdir("out"));
    const QString source = dir.filePath("stage/archive.zip"), target = dir.filePath("out/archive.zip");
    QVERIFY(writeLooknfeel(source, "incoming")); QVERIFY(writeLooknfeel(target, "existing"));
    QString why;
    QVERIFY(writeTagsXattr(source, {"incoming"}, &why)); putIndexRow(source, {"incoming"});
    QVERIFY(writeTagsXattr(target, {"existing"}, &why)); putIndexRow(target, {"existing"});
    JournalDriver driver;
    QSignalSpy conflict(&driver.journal, &UndoJournal::conflictRequested);
    driver.journal.install({source}, dir.filePath("out"), "compress", driver.receipt());
    QTRY_VERIFY(!conflict.isEmpty() || driver.finished);
    QCOMPARE(conflict.size(), 1);
    driver.journal.resolveConflict(policy, true);
    QTRY_VERIFY(driver.finished);
    QCOMPARE(driver.ok, policy != "cancel");
    if (policy == "skip" || policy == "cancel") { QCOMPARE(archiveTestBytes(target), QByteArray("existing")); QVERIFY(!driver.journal.available()); return; }
    const QString landed = policy == "keep" ? dir.filePath("out/archive (1).zip") : target;
    QCOMPARE(archiveTestBytes(landed), QByteArray("incoming"));
    QVERIFY(QDir(dir.filePath("stage")).removeRecursively());
    driver.undo(); QTRY_VERIFY(driver.finished); QVERIFY2(driver.ok, qPrintable(driver.notice));
    QCOMPARE(archiveTestBytes(target), QByteArray("existing"));
    Store store(defaultIndexPath()); QVERIFY(store.load(&why));
    QCOMPARE(store.get(target).tags, QStringList{"existing"});
    QVERIFY(!store.contains(source));
}

void TestStudio::stopsArchivePublicationAndAppliesConflictPolicy()
{
    QTemporaryDir fixture;
    const QDir dir(fixture.path());
    QVERIFY(dir.mkdir("stage")); QVERIFY(dir.mkdir("out"));
    JournalDriver driver;
    QStringList sources;
    for (const QString name : {"first", "second"}) {
        sources << dir.filePath("stage/" + name);
        QVERIFY(writeLooknfeel(sources.last(), "incoming"));
        QVERIFY(writeLooknfeel(dir.filePath("out/" + name), "existing"));
    }
    QSignalSpy conflicts(&driver.journal, &UndoJournal::conflictRequested);
    driver.journal.install(sources, dir.filePath("out"), "extract", driver.receipt());
    QTRY_COMPARE(conflicts.size(), 1);
    connect(&driver.journal, &UndoJournal::conflictRequested, &driver.journal, [&] {
        if (conflicts.size() > 1) driver.journal.resolveConflict("replace", false);
    });
    driver.journal.resolveConflict("replace", true);
    QTRY_VERIFY(driver.finished); QVERIFY2(driver.ok, qPrintable(driver.notice));
    QCOMPARE(conflicts.size(), 1);
    for (const QString name : {"first", "second"}) QCOMPARE(archiveTestBytes(dir.filePath("out/" + name)), QByteArray("incoming"));
    driver.undo(); QTRY_VERIFY(driver.finished); QVERIFY(driver.ok);
    for (const QString name : {"first", "second"}) QCOMPARE(archiveTestBytes(dir.filePath("out/" + name)), QByteArray("existing"));
    QVERIFY(writeLooknfeel(dir.filePath("stage/a"), "one"));
    QVERIFY(writeLooknfeel(dir.filePath("stage/b"), "two"));
    driver.finished = false;
    bool canceled = false;
    driver.journal.install({dir.filePath("stage/a"), dir.filePath("stage/b")}, dir.filePath("out"), "extract", driver.receipt(),
        [&] { return canceled; }, [&](int done, int) { if (done == 1) canceled = true; });
    QTRY_VERIFY(driver.finished); QVERIFY(!driver.ok);
    QCOMPARE(archiveTestBytes(dir.filePath("out/a")), QByteArray("one"));
    QVERIFY(!QFileInfo::exists(dir.filePath("out/b")));
    driver.undo(); QTRY_VERIFY(driver.finished); QVERIFY(driver.ok);
    QVERIFY(!QFileInfo::exists(dir.filePath("out/a")));
}

void TestStudio::refusesUnsafeArchiveDestinations()
{
    JournalDriver tools;
    QTemporaryDir fixture;
    const QDir dir(fixture.path());
    QVERIFY(dir.mkdir("source")); QVERIFY(dir.mkdir("out"));
    QVERIFY(writeLooknfeel(dir.filePath("source/file"), "payload"));
    QVERIFY(::symlink("out", QFile::encodeName(dir.filePath("link")).constData()) == 0);
    ArchiveJob job;
    QSignalSpy ready(&job, &ArchiveJob::ready), finished(&job, &ArchiveJob::finished);
    job.compress({dir.filePath("source")}, dir.filePath("source/nested.zip"), "zip");
    QTRY_COMPARE(finished.size(), 1);
    QVERIFY(!finished.last()[0].toBool());
    QVERIFY(finished.last()[1].toString().contains("outside"));
    job.compress({dir.filePath("source/file")}, dir.filePath("link/archive.zip"), "zip");
    QTRY_VERIFY(finished.size() == 2 || !ready.isEmpty());
    QVERIFY(ready.isEmpty());
    QCOMPARE(finished.size(), 2);
    QVERIFY(!finished.last()[0].toBool());
    QVERIFY(finished.last()[1].toString().contains("symbolic links"));
    QVERIFY(ready.isEmpty());
    QVERIFY(dir.entryList({".omafiles-archive-*"}, QDir::Dirs | QDir::Hidden).isEmpty());
    QVERIFY(QDir(dir.filePath("out")).entryList(QDir::AllEntries | QDir::NoDotAndDotDot).isEmpty());
}

void TestStudio::navigatesArchiveControls_data()
{
    QTest::addColumn<QSize>("size"); QTest::addColumn<bool>("extracting");
    for (const QSize size : {QSize(1180, 720), QSize(420, 300)})
        for (bool extracting : {false, true}) QTest::newRow(qPrintable(QString::number(size.width()) + QString::number(extracting))) << size << extracting;
}
void TestStudio::navigatesArchiveControls()
{
    QFETCH(QSize, size); QFETCH(bool, extracting);
    const QSize original = window()->size(), minimum = window()->minimumSize();
    const auto restore = qScopeGuard([&] { key(Qt::Key_Escape); window()->setMinimumSize(minimum); window()->resize(original); });
    window()->setMinimumSize(QSize(1, 1));
    window()->resize(size); focusRow("one.txt");
    QVERIFY(QMetaObject::invokeMethod(window(), "startArchive", Q_ARG(QVariant, extracting), Q_ARG(QVariant, QString())));
    auto *sheet = shownItem(window()->contentItem(), "archiveSheet"); QVERIFY(sheet);
    auto *field = shownItem(sheet, "archiveDestination"); QVERIFY(field);
    auto *origin = window()->activeFocusItem(); QVERIFY(field->isAncestorOf(origin));
    const QStringList controls = extracting ? QStringList{"archiveCancel", "archiveStart", "archiveDestination"}
        : QStringList{"archiveZip", "archiveGzip", "archiveZstd", "archiveCancel", "archiveStart", "archiveDestination"};
    for (const QString &name : controls) {
        key(Qt::Key_Tab);
        auto *control = shownItem(sheet, name); QVERIFY(control);
        auto *focused = window()->activeFocusItem(); QVERIFY(focused == control || control->isAncestorOf(focused));
        QTRY_VERIFY2(QRectF(QPointF(), size).contains(control->mapRectToScene(QRectF(0, 0, control->width(), control->height()))), qPrintable(name));
        QTRY_VERIFY2(hasFocusOutline(window(), control, m_theme->accent()), qPrintable(name));
    }
    QCOMPARE(window()->activeFocusItem(), origin);
    for (int i = 0; i < controls.size(); ++i) key(Qt::Key_Backtab, Qt::ShiftModifier);
    QCOMPARE(window()->activeFocusItem(), origin);
    if (!extracting) {
        auto *gzip = shownItem(sheet, "archiveGzip"); QVERIFY(gzip);
        gzip->forceActiveFocus(); key(Qt::Key_Space);
        QCOMPARE(sheet->property("format").toString(), QStringLiteral("tar.gz"));
        QVERIFY(field->property("text").toString().endsWith(".tar.gz"));
        QVERIFY(tapItem(window(), shownItem(sheet, "archiveZstd"), false, false));
        QCOMPARE(sheet->property("format").toString(), QStringLiteral("tar.zst"));
    }
    QVERIFY(tapItem(window(), shownItem(sheet, "archiveCancel"), false, true));
    QTRY_VERIFY(!sheet->isVisible());
    QVERIFY(!m_index->archive()->busy());
}


static QString deviceClientHelper(QTemporaryDir &directory, const QByteArray &body)
{
    const QString path = directory.filePath("devices.py");
    QFile script(path);
    if (!script.open(QIODevice::WriteOnly)) return {};
    script.write("#!/usr/bin/python3\nimport sys,json\n" + body);
    script.close();
    if (!script.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner)) return {};
    return path;
}

static const QByteArray deviceClientRow = R"({"id":"usb:test","name":"Test USB","path":"","phone":false,"mounted":false,"canMount":true,"canUnmount":false,"canEject":true})";

void TestStudio::monitorsDeviceHotplugAndOpensMountedPaths()
{
    QTemporaryDir temporary; QVERIFY(temporary.isValid());
    const auto helper = deviceClientHelper(temporary,
        "row=json.loads('" + deviceClientRow + "')\n"
        "print(json.dumps({'devices':[row]}),flush=True)\n"
        "for line in sys.stdin:\n"
        " command=json.loads(line)\n"
        " if command['action']=='refresh':\n"
        "  print(json.dumps({'devices':[]}),flush=True)\n"
        " else:\n"
        "  row.update(path='/mnt/test USB',mounted=True,canMount=False,canUnmount=True)\n"
        "  print(json.dumps({'devices':[row]}),flush=True)\n"
        "  print(json.dumps(dict(command,finished=True,ok=True,path=row['path'])),flush=True)\n");
    QVERIFY(!helper.isEmpty());
    Devices devices;
    QCOMPARE(devices.entries().size(), 0); QVERIFY(!devices.busy());
    QSignalSpy storage(&devices, &Devices::storageChanged), opened(&devices, &Devices::openRequested);
    auto *monitor = devices.findChild<QProcess *>(); QVERIFY(monitor);
    QCOMPARE(monitor->state(), QProcess::NotRunning);
    devices.start(helper);
    QTRY_COMPARE_WITH_TIMEOUT(devices.entries().size(), 1, asyncTimeout);
    QCOMPARE(devices.entries().first().toMap().value("name").toString(), QStringLiteral("Test USB"));
    devices.act("usb:test", "open"); QVERIFY(devices.busy());
    devices.act("usb:test", "eject");
    QTRY_COMPARE_WITH_TIMEOUT(opened.size(), 1, asyncTimeout);
    QCOMPARE(opened.first().first().toString(), QStringLiteral("/mnt/test USB"));
    QVERIFY(!devices.busy());
    QVERIFY(devices.entries().first().toMap().value("mounted").toBool());
    devices.refresh(); QTRY_VERIFY_WITH_TIMEOUT(devices.entries().isEmpty(), asyncTimeout);
    QVERIFY(storage.size() >= 3);
    devices.act("usb:test", "mount");
    QCOMPARE(devices.status(), QStringLiteral("The device is no longer connected."));
}

void TestStudio::cancelsDeviceOperationsAndPreservesServiceRefusals()
{
    QTemporaryDir temporary; QVERIFY(temporary.isValid());
    const auto helper = deviceClientHelper(temporary,
        "print(json.dumps({'devices':[json.loads('" + deviceClientRow + "')]}),flush=True)\n"
        "pending=None\n"
        "for line in sys.stdin:\n"
        " command=json.loads(line)\n"
        " if command['action']=='open': pending=command\n"
        " elif command['action']=='cancel':\n"
        "  print(json.dumps(dict(pending,finished=True,ok=False,reason='Operation was canceled')),flush=True)\n"
        " else:\n"
        "  print(json.dumps(dict(command,finished=True,ok=False,reason='Device is busy: a document is open')),flush=True)\n");
    Devices devices; QSignalSpy opened(&devices, &Devices::openRequested);
    devices.start(helper); QTRY_COMPARE_WITH_TIMEOUT(devices.entries().size(), 1, asyncTimeout);
    devices.act("usb:test", "open"); QVERIFY(devices.busy());
    devices.act("usb:test", "cancel");
    QTRY_VERIFY_WITH_TIMEOUT(!devices.busy(), asyncTimeout);
    QCOMPARE(devices.status(), QStringLiteral("Operation was canceled"));
    devices.act("usb:test", "unmount");
    QTRY_VERIFY_WITH_TIMEOUT(!devices.busy(), asyncTimeout);
    QCOMPARE(devices.status(), QStringLiteral("Device is busy: a document is open"));
    QCOMPARE(devices.entries().size(), 1); QVERIFY(opened.isEmpty());
}

void TestStudio::refusesInvalidDeviceProtocol_data()
{
    QTest::addColumn<QByteArray>("output");
    QTest::addColumn<QString>("reason");
    QTest::newRow("invalid-json") << QByteArray("broken\n") << QString("invalid data");
    QTest::newRow("wrong-list-type") << QByteArray("{\"devices\":{}}\n") << QString("invalid data");
    QTest::newRow("unknown-message") << QByteArray("{}\n") << QString("invalid data");
    QTest::newRow("unbounded-frame") << QByteArray(1024 * 1024 + 1, 'x') << QString("too much data");
    QJsonObject row = QJsonDocument::fromJson(deviceClientRow).object();
    row["path"] = "relative/path";
    QTest::newRow("relative-path") << QJsonDocument(QJsonObject{{"devices", QJsonArray{row}}}).toJson(QJsonDocument::Compact) + '\n' << QString("invalid device path or identity");
    row["path"] = ""; row["canMount"] = "yes";
    QTest::newRow("invalid-capability") << QJsonDocument(QJsonObject{{"devices", QJsonArray{row}}}).toJson(QJsonDocument::Compact) + '\n' << QString("invalid device path or identity");
    row["canMount"] = true;
    QTest::newRow("duplicate-id") << QJsonDocument(QJsonObject{{"devices", QJsonArray{row, row}}}).toJson(QJsonDocument::Compact) + '\n' << QString("invalid device path or identity");
}

void TestStudio::refusesInvalidDeviceProtocol()
{
    QFETCH(QByteArray, output); QFETCH(QString, reason);
    QTemporaryDir temporary; QVERIFY(temporary.isValid());
    const auto helper = deviceClientHelper(temporary,
        "sys.stdout.buffer.write(bytes.fromhex('" + output.toHex() + "'))\n"
        "sys.stdout.flush()\n"
        "for line in sys.stdin: pass\n");
    Devices devices;
    QSignalSpy notices(&devices, &Devices::notice), opened(&devices, &Devices::openRequested);
    devices.start(helper);
    QTRY_VERIFY_WITH_TIMEOUT(devices.status().contains(reason), asyncTimeout);
    QTRY_VERIFY_WITH_TIMEOUT(!notices.isEmpty(), asyncTimeout);
    auto *process = devices.findChild<QProcess *>(); QVERIFY(process);
    QTRY_COMPARE_WITH_TIMEOUT(process->state(), QProcess::NotRunning, asyncTimeout);
    QCOMPARE(notices.first().first().toString(), devices.status());
    QVERIFY(devices.entries().isEmpty()); QVERIFY(!devices.busy()); QVERIFY(opened.isEmpty());
}

void TestStudio::reportsDeviceHelperStartupAndExitFailures()
{
    QTemporaryDir temporary; QVERIFY(temporary.isValid());
    Devices missing;
    missing.start(temporary.filePath("missing-helper"));
    QTRY_VERIFY_WITH_TIMEOUT(missing.status().startsWith("Device helper could not start:"), asyncTimeout);
    Devices exited;
    const auto helper = deviceClientHelper(temporary, "sys.exit(3)\n");
    exited.start(helper);
    QTRY_COMPARE_WITH_TIMEOUT(exited.status(), QStringLiteral("Device monitor stopped. Reopen the window to reconnect."), asyncTimeout);
    QVERIFY(exited.entries().isEmpty()); QVERIFY(!exited.busy());
}


void TestStudio::refusesUnsafeDeviceOpenReplies_data()
{
    QTest::addColumn<QString>("path");
    QTest::newRow("relative") << QString("relative");
    QTest::newRow("parent-component") << QString("/mnt/../elsewhere");
    QTest::newRow("embedded-nul") << QString("/mnt/usb") + QChar(0) + "suffix";
    QTest::newRow("empty") << QString();
}

void TestStudio::refusesUnsafeDeviceOpenReplies()
{
    QFETCH(QString, path);
    QTemporaryDir temporary; QVERIFY(temporary.isValid());
    const QByteArray response = QJsonDocument(QJsonObject{{"finished", true}, {"ok", true},
        {"id", "usb:test"}, {"action", "open"}, {"path", path}}).toJson(QJsonDocument::Compact);
    const auto helper = deviceClientHelper(temporary,
        "print(json.dumps({'devices':[json.loads('" + deviceClientRow + "')]}),flush=True)\n"
        "for line in sys.stdin:\n"
        " sys.stdout.buffer.write(bytes.fromhex('" + response.toHex() + "')+b'\\n')\n"
        " sys.stdout.flush()\n");
    Devices devices; QSignalSpy opened(&devices, &Devices::openRequested);
    devices.start(helper); QTRY_COMPARE_WITH_TIMEOUT(devices.entries().size(), 1, asyncTimeout);
    devices.act("usb:test", "open");
    QTRY_COMPARE_WITH_TIMEOUT(devices.status(), QStringLiteral("Device monitor returned an invalid mount path."), asyncTimeout);
    QVERIFY(opened.isEmpty()); QVERIFY(!devices.busy());
}


void TestStudio::navigatesDevicesAndTheirActions()
{
    QTemporaryDir temporary; QVERIFY(temporary.isValid());
    QJsonArray rows;
    for (int i = 0; i < 24; ++i) {
        auto row = QJsonDocument::fromJson(deviceClientRow).object();
        row["id"] = QString("usb:%1").arg(i); row["name"] = QString("Drive %1").arg(i);
        row["canEject"] = true; row["canUnmount"] = true;
        rows.append(row);
    }
    const auto data = QJsonDocument(rows).toJson(QJsonDocument::Compact);
    const auto path = QJsonDocument(QJsonArray{m_tree.path()}).toJson(QJsonDocument::Compact);
    const auto helper = deviceClientHelper(temporary,
        "rows=json.loads(bytes.fromhex('" + data.toHex() + "'))\n"
        "print(json.dumps({'devices':rows}),flush=True)\n"
        "for line in sys.stdin:\n"
        " c=json.loads(line)\n"
        " if c['action']=='open':\n"
        "  print(json.dumps(dict(c,finished=True,ok=True,path=json.loads(bytes.fromhex('" + path.toHex() + "'))[0])),flush=True)\n"
        " else:\n"
        "  rows[-1]['mounted']=True; rows[-1]['canMount']=False\n"
        "  print(json.dumps({'devices':rows}),flush=True)\n"
        "  print(json.dumps(dict(c,finished=True,ok=False,reason='Drive is busy')),flush=True)\n");
    auto *devices = m_engine->singletonInstance<Devices *>("omafiles", "Devices");
    auto *process = devices->findChild<QProcess *>(); QVERIFY(process);
    if (process->state() != QProcess::NotRunning) { process->kill(); QTRY_COMPARE_WITH_TIMEOUT(process->state(), QProcess::NotRunning, asyncTimeout); }
    const auto stop = qScopeGuard([&] { process->kill(); process->waitForFinished(asyncTimeout); });
    devices->start(helper); QTRY_COMPARE_WITH_TIMEOUT(devices->entries().size(), 24, asyncTimeout);
    key('d');
    QTRY_COMPARE_WITH_TIMEOUT(window()->property("focusRegion").toString(), QString("devices"), asyncTimeout);
    auto *list = qobject_cast<QQuickItem *>(window()->property("activeDevicesList").value<QObject *>());
    QVERIFY(list); QVERIFY(list->hasActiveFocus());
    key(Qt::Key_End); QCOMPARE(list->property("currentIndex").toInt(), 23);
    key(Qt::Key_Home); QCOMPARE(list->property("currentIndex").toInt(), 0);
    key('j'); QCOMPARE(list->property("currentIndex").toInt(), 1);
    key('k'); QCOMPARE(list->property("currentIndex").toInt(), 0);
    key(Qt::Key_PageDown); QVERIFY(list->property("currentIndex").toInt() > 0);
    key(Qt::Key_PageUp); QCOMPARE(list->property("currentIndex").toInt(), 0);
    for (const auto &press : {QPair<Qt::Key, Qt::KeyboardModifiers>{Qt::Key_M, Qt::NoModifier}, QPair<Qt::Key, Qt::KeyboardModifiers>{Qt::Key_Menu, Qt::NoModifier},
                              QPair<Qt::Key, Qt::KeyboardModifiers>{Qt::Key_F10, Qt::ShiftModifier}}) {
        key(press.first, press.second);
        QTRY_VERIFY_WITH_TIMEOUT(window()->findChild<QObject *>("deviceMenu")->property("visible").toBool(), asyncTimeout);
        QVERIFY(menuLabels("deviceMenu").contains("Mount"));
        QVERIFY(menuLabels("deviceMenu").contains("Unmount"));
        QVERIFY(menuLabels("deviceMenu").contains("Eject safely"));
        key(Qt::Key_Escape); QTRY_VERIFY_WITH_TIMEOUT(list->hasActiveFocus(), asyncTimeout);
    }
    key(Qt::Key_End);
    key('m');
    auto *menu = window()->findChild<QObject *>("deviceMenu");
    QVERIFY(QMetaObject::invokeMethod(menu, "chosen", Q_ARG(QString, "unmount")));
    QMetaObject::invokeMethod(menu, "close");
    QTRY_COMPARE_WITH_TIMEOUT(devices->status(), QString("Drive is busy"), asyncTimeout);
    QCOMPARE(m_index->notice(), QString("Drive is busy"));
    QCOMPARE(list->property("currentIndex").toInt(), 23);
    for (const auto press : {Qt::Key_Space, Qt::Key_Return, Qt::Key_L}) {
        key('d');
        const int before = m_workspace->paneAtIndex(0)->tabCount();
        key(press);
        QTRY_VERIFY_WITH_TIMEOUT(!devices->busy(), asyncTimeout);
        QTRY_COMPARE_WITH_TIMEOUT(m_workspace->paneAtIndex(0)->tabCount(), before + 1, asyncTimeout);
        QTRY_COMPARE_WITH_TIMEOUT(cwd(), m_tree.path(), asyncTimeout);
    }
    QTRY_COMPARE_WITH_TIMEOUT(window()->property("focusRegion").toString(), QString("list"), asyncTimeout);
    key('d'); key(Qt::Key_Escape);
    QCOMPARE(window()->property("focusRegion").toString(), QString("list"));
}

void TestStudio::opensDeviceMenusByMouseAndTouch()
{
    const QSize originalSize = window()->size(), originalMinimum = window()->minimumSize();
    const auto restoreWindow = qScopeGuard([&] {
        window()->setMinimumSize(originalMinimum); window()->resize(originalSize);
    });
    QTemporaryDir temporary; QVERIFY(temporary.isValid());
    const auto helper = deviceClientHelper(temporary,
        "print(json.dumps({'devices':[json.loads('" + deviceClientRow + "')]}),flush=True)\n"
        "for line in sys.stdin: pass\n");
    auto *devices = m_engine->singletonInstance<Devices *>("omafiles", "Devices");
    auto *process = devices->findChild<QProcess *>(); QVERIFY(process);
    if (process->state() != QProcess::NotRunning) { process->kill(); QTRY_COMPARE_WITH_TIMEOUT(process->state(), QProcess::NotRunning, asyncTimeout); }
    const auto stop = qScopeGuard([&] { process->kill(); process->waitForFinished(asyncTimeout); });
    devices->start(helper); QTRY_COMPARE_WITH_TIMEOUT(devices->entries().size(), 1, asyncTimeout);
    window()->setMinimumSize(QSize(0, 0)); window()->resize(480, 320);
    key('d');
    auto *list = qobject_cast<QQuickItem *>(window()->property("activeDevicesList").value<QObject *>());
    QVERIFY(list);
    QQuickItem *row = nullptr;
    QTRY_VERIFY_WITH_TIMEOUT((row = viewItemAt(list, 0)) != nullptr, asyncTimeout);
    const auto point = row->mapToScene(QPointF(30, row->height() / 2)).toPoint();
    QTest::mouseClick(window(), Qt::RightButton, Qt::NoModifier, point);
    auto *menu = window()->findChild<QObject *>("deviceMenu");
    QTRY_VERIFY_WITH_TIMEOUT(menu->property("visible").toBool(), asyncTimeout);
    key(Qt::Key_Escape);
    for (const bool pen : {false, true}) {
        QVERIFY(tapItem(window(), row, true, pen));
        QTRY_VERIFY_WITH_TIMEOUT(menu->property("visible").toBool(), asyncTimeout);
        QVERIFY(!devices->busy());
        key(Qt::Key_Escape); QTRY_VERIFY_WITH_TIMEOUT(list->hasActiveFocus(), asyncTimeout);
    }
    auto *button = row->findChild<QQuickItem *>("deviceMenuButton"); QVERIFY(button);
    auto at = button->mapToScene(QPointF(button->width() / 2, button->height() / 2)).toPoint();
    QVERIFY(window()->contentItem()->contains(at));
    static auto *touch = QTest::createTouchDevice();
    auto sequence = QTest::touchEvent(window(), touch, false);
    sequence.press(0, at, window()).commit(); sequence.release(0, at, window()).commit();
    QTRY_VERIFY_WITH_TIMEOUT(menu->property("visible").toBool(), asyncTimeout);
    QVERIFY(!devices->busy());
    key(Qt::Key_Escape); QTRY_VERIFY_WITH_TIMEOUT(list->hasActiveFocus(), asyncTimeout);
    key(Qt::Key_Tab); key(Qt::Key_Backtab);
    QVERIFY(list->hasActiveFocus());
}

void TestStudio::refusesPointerInputWhenPolishRetiresTarget_data()
{
    waitsForLayoutBeforeInjectingPointerInput_data();
}

void TestStudio::refusesPointerInputWhenPolishRetiresTarget()
{
    QFETCH(bool, pen);
    QQuickWindow scene;
    scene.resize(320, 200);
    scene.show();
    QVERIFY(QTest::qWaitForWindowExposed(&scene, asyncTimeout));
    QScopedPointer<QQuickItem> target(pointerControl(m_engine, &scene, 1));
    QScopedPointer<QQuickItem> oldSpot(pointerControl(m_engine, &scene, 0));
    QVERIFY(target && oldSpot);
    QSignalSpy staleInput(oldSpot.data(), SIGNAL(activated()));
    QVERIFY(staleInput.isValid());
    ScenePolish retirement(scene.contentItem());
    retirement.action = [&] { target.reset(); };
    QVERIFY(QQuickTest::qWaitForPolish(&scene, asyncTimeout));
    const QPointer<QQuickItem> alive(target.data());
    InputPressObserver input;
    scene.installEventFilter(&input);
    QVERIFY(target && !retirement.completed);
    retirement.polish();
    QVERIFY(QQuickTest::qIsPolishScheduled(&retirement));
    QVERIFY(!tapItem(&scene, target.data(), false, pen));
    QVERIFY(retirement.completed);
    QVERIFY(alive.isNull());
    QCOMPARE(input.presses, 0);
    QCOMPARE(staleInput.count(), 0);
}

void TestStudio::includesQueuedDeclarationRefreshInLoading()
{
    QTemporaryDir fixture;
    QVERIFY(fixture.isValid());
    const auto oldConfig = qgetenv("XDG_CONFIG_HOME");
    const auto oldData = qgetenv("XDG_DATA_HOME");
    const auto oldRoots = roots();
    const auto restore = qScopeGuard([&] {
        qputenv("XDG_CONFIG_HOME", oldConfig);
        qputenv("XDG_DATA_HOME", oldData);
        setRoots(oldRoots);
    });
    qputenv("XDG_CONFIG_HOME", fixture.filePath("config").toUtf8());
    qputenv("XDG_DATA_HOME", fixture.filePath("data").toUtf8());
    QVERIFY(QDir().mkpath(QFileInfo(defaultRootsPath()).absolutePath()));
    IndexModel model;
    QTRY_VERIFY(!model.loading());
    QVector<bool> states;
    QVector<bool> declarationsLoaded;
    connect(&model, &IndexModel::loadingChanged, &model, [&] {
        states.append(model.loading());
        declarationsLoaded.append(bool(rootById("queued")));
    });
    QString error;
    QVERIFY(saveRoots(defaultRootsPath(), {Root{"queued", fixture.filePath("usb"), false, {}}}, &error));
    QTRY_VERIFY(!states.isEmpty());
    QVERIFY(states.first());
    QVERIFY(!declarationsLoaded.first());
    QTRY_VERIFY(!model.loading());
    QVERIFY(!states.last());
}

void TestStudio::watchesDeclaredRootsAcrossCreationAndReplacement()
{
    QTemporaryDir fixture;
    QVERIFY(fixture.isValid());
    const auto oldConfig = qgetenv("XDG_CONFIG_HOME");
    const auto oldData = qgetenv("XDG_DATA_HOME");
    const auto oldRoots = roots();
    const auto restore = qScopeGuard([&] {
        qputenv("XDG_CONFIG_HOME", oldConfig);
        qputenv("XDG_DATA_HOME", oldData);
        setRoots(oldRoots);
    });
    qputenv("XDG_CONFIG_HOME", fixture.filePath("config").toUtf8());
    qputenv("XDG_DATA_HOME", fixture.filePath("data").toUtf8());
    QVERIFY(QDir().mkpath(fixture.filePath("config")));
    IndexModel model;
    QTRY_VERIFY(!model.loading());
    QVERIFY(model.error().isEmpty());
    const QString path = defaultRootsPath();
    QVERIFY(QDir().mkpath(QFileInfo(path).absolutePath()));
    QString error;
    const QString usb = fixture.filePath("usb");
    QVERIFY(QDir().mkpath(usb));
    QVERIFY(saveRoots(path, {Root{"usb", usb, false, "marker"}}, &error));
    QTRY_VERIFY(model.error().contains("root usb is declared and not here"));
    QVERIFY(writeLooknfeel(usb + "/marker", "present"));
    QVERIFY(saveRoots(path, {Root{"usb", usb, false, {}}}, &error));
    QTRY_VERIFY(model.error().isEmpty());
    QVERIFY(saveRoots(path, {Root{"usb", usb, false, "other-marker"}}, &error));
    QTRY_VERIFY(model.error().contains("root usb is declared and not here"));
    model.setNotice("Completed with the previous declarations");
    QVERIFY(saveRoots(path, {Root{"usb", usb, false, "another-marker"}}, &error));
    QTRY_VERIFY(model.notice().isEmpty());
    QVERIFY(QFile::remove(path));
    QTRY_VERIFY(model.error().isEmpty());
    QVERIFY(saveRoots(path, {Root{"second", usb, false, "other-marker"}}, &error));
    QTRY_VERIFY(model.error().contains("root second is declared and not here"));
    QVERIFY(writeLooknfeel(path, "not json\n"));
    QTRY_VERIFY(model.error().contains("roots: malformed"));
    QVERIFY(!model.error().contains("store: malformed"));
    QVERIFY(saveRoots(path, {}, &error));
    QTRY_VERIFY(model.error().isEmpty());
}

void TestStudio::reloadsExternalRootsBeforeReadingIndex()
{
    QTemporaryDir fixture;
    QVERIFY(fixture.isValid());
    const auto oldRoots = roots();
    const auto restore = qScopeGuard([&] { declareRoots(oldRoots); writeIndex(); m_index->reload(); });
    const QString usb = fixture.filePath("usb");
    QVERIFY(writeLooknfeel(usb + "/marker", "present"));
    QVERIFY(writeLooknfeel(usb + "/tagged.txt", "payload"));
    const QString cli = QDir(QCoreApplication::applicationDirPath()).absoluteFilePath("../../build/bin/omafiles");
    QProcess command;
    command.start(cli, {"roots", "add", "--id", "usb", "--check", "marker", "--", usb});
    QVERIFY(command.waitForFinished(asyncTimeout));
    QCOMPARE(command.exitCode(), 0);
    command.start(cli, {"tag", "add", "--tag", "usbtag", "--", usb + "/tagged.txt"});
    QVERIFY(command.waitForFinished(asyncTimeout));
    QCOMPARE(command.exitCode(), 0);
    QVERIFY(QFile::remove(usb + "/marker"));
    key(Qt::Key_F5);
    QTRY_VERIFY(m_index->error().contains("root usb is declared and not here"));
    QVERIFY2(!m_index->error().contains("malformed"), qPrintable(m_index->error()));
    QVERIFY(m_index->fileCount() > 0);
    QVERIFY(!indexModelHasTag(m_index, "usbtag"));
    QCOMPARE(itemsNamed("statusBar").first()->property("message").toString(), m_index->error());
    QVERIFY(writeLooknfeel(usb + "/marker", "present"));
    key(Qt::Key_F5);
    QTRY_VERIFY(m_index->error().isEmpty());
    QVERIFY(indexModelHasTag(m_index, "usbtag"));
    QVERIFY(QFile::remove(usb + "/marker"));
    key(Qt::Key_F5);
    QTRY_VERIFY(m_index->error().contains("root usb is declared and not here"));
}

void TestStudio::warnsAboutAbsentRootsWithoutDevices_data()
{
    QTest::addColumn<bool>("mixedIndex");
    QTest::newRow("empty-index") << false;
    QTest::newRow("present-and-absent-rows") << true;
}

void TestStudio::warnsAboutAbsentRootsWithoutDevices()
{
    QFETCH(bool, mixedIndex);
    QTemporaryDir fixture;
    QVERIFY(fixture.isValid());
    const auto oldConfig = qgetenv("XDG_CONFIG_HOME");
    const auto oldData = qgetenv("XDG_DATA_HOME");
    const auto oldRoots = roots();
    const auto restore = qScopeGuard([&] {
        qputenv("XDG_CONFIG_HOME", oldConfig);
        qputenv("XDG_DATA_HOME", oldData);
        setRoots(oldRoots);
    });
    qputenv("XDG_CONFIG_HOME", fixture.filePath("config").toUtf8());
    qputenv("XDG_DATA_HOME", fixture.filePath("data").toUtf8());
    QVERIFY(QDir().mkpath(QFileInfo(defaultRootsPath()).absolutePath()));
    const QString usb = fixture.filePath("usb");
    QVERIFY(writeLooknfeel(usb + "/tagged.txt", "payload"));
    QString error;
    QVERIFY(saveRoots(defaultRootsPath(), {Root{"usb", usb, false, "marker"}}, &error));
    if (mixedIndex) {
        const QString local = fixture.filePath("local.txt");
        QVERIFY(writeLooknfeel(local, "local"));
        QVERIFY(writeLooknfeel(defaultIndexPath(),
            QString("{\"path\":\"%1\",\"tags\":[\"local\"]}\n"
                    "{\"root\":\"usb\",\"path\":\"tagged.txt\",\"tags\":[\"away\"]}\n").arg(local)));
    }
    IndexModel model;
    QTRY_VERIFY(!model.loading());
    QVERIFY2(model.error().contains("root usb is declared and not here"), qPrintable(model.error()));
    QVERIFY(!model.error().contains("malformed"));
    QCOMPARE(model.fileCount(), mixedIndex ? 1 : 0);
    QCOMPARE(model.tagCount(), mixedIndex ? 1 : 0);
    model.setNotice("Finished operation");
    model.reload();
    QTRY_VERIFY(!model.loading());
    QVERIFY(model.error().contains("root usb is declared and not here"));
    QVERIFY(writeLooknfeel(usb + "/marker", "present"));
    model.reload();
    QTRY_VERIFY(!model.loading());
    QVERIFY(model.error().isEmpty());
    QCOMPARE(model.fileCount(), mixedIndex ? 2 : 0);
    QVERIFY(QFile::remove(usb + "/marker"));
    model.reload();
    QTRY_VERIFY(!model.loading());
    QVERIFY(model.error().contains("root usb is declared and not here"));
}

void TestStudio::keepsStorageWarningsAheadOfCompletedReceipts()
{
    QTemporaryDir fixture;
    QVERIFY(fixture.isValid());
    const auto oldRoots = roots();
    const auto restore = qScopeGuard([&] { declareRoots(oldRoots); m_index->reload(); });
    declareRoots({Root{"usb", fixture.filePath("usb"), false, "marker"}});
    m_index->reload();
    settle();
    auto *status = itemsNamed("statusBar").first();
    const QString warning = m_index->error();
    QVERIFY(warning.contains("root usb is declared and not here"));
    QCOMPARE(status->property("message").toString(), warning);
    const QString source = fixture.filePath("source.txt");
    QVERIFY(writeLooknfeel(source, "payload"));
    QVERIFY(QDir().mkpath(fixture.filePath("out")));
    JournalDriver driver;
    m_index->copyInto({source}, fixture.filePath("out"));
    QTRY_VERIFY(!m_index->operationBusy());
    QVERIFY(m_index->notice().contains("copied 1 file"));
    QCOMPARE(status->property("message").toString(), warning);
    m_index->compress({source}, fixture.filePath("source.zip"), "zip");
    QTRY_VERIFY(!m_index->archive()->busy());
    QVERIFY(m_index->notice().contains("published 1 entries"));
    QCOMPARE(status->property("message").toString(), warning);
    const QString trash = QDir(dataHome()).filePath("Trash");
    QVERIFY(writeLooknfeel(trash + "/files/r30.txt", "trashed"));
    QVERIFY(writeLooknfeel(trash + "/info/r30.txt.trashinfo",
        "[Trash Info]\nPath=" + fixture.filePath("restored.txt") + "\nDeletionDate=2026-10-12T12:00:00\n"));
    m_workspace->openTabIn(0, "trash", {});
    settle();
    QObject *view = window()->property("activeTrash").value<QObject *>();
    QVERIFY(view);
    auto *model = qobject_cast<TrashModel *>(view->property("model").value<QObject *>());
    QVERIFY(model);
    QTRY_VERIFY(!model->loading());
    const int row = model->rowForPath(trash + "/files/r30.txt");
    QVERIFY(row >= 0);
    model->restore(row);
    QTRY_VERIFY(!model->busy() && !model->loading());
    QVERIFY(model->message().contains("Restored 1"));
    QCOMPARE(status->property("message").toString(), warning);
}

void TestStudio::refreshesDeclaredRootsWhenDevicesAppear()
{
    QTest::failOnWarning(QRegularExpression("Cannot set activeFocusOnTab"));
    QTemporaryDir temporary; QVERIFY(temporary.isValid());
    const QString marker = m_tree.filePath("device-available");
    QFile::remove(marker);
    const auto rootsBefore = declareRoots({Root{"test-device", m_tree.path(), true, "device-available"}});
    const auto restore = qScopeGuard([&] {
        QThreadPool::globalInstance()->waitForDone(); declareRoots(rootsBefore); QFile::remove(marker);
    });
    m_index->reload();
    settle();
    QCOMPARE(m_index->fileCount(), 0);
    QVERIFY(m_index->error().contains("root that is not here"));
    QCOMPARE(m_index->tagCount(), 0);
    m_index->setNotice("Device operation completed");
    QCOMPARE(itemsNamed("statusBar").first()->property("message").toString(), m_index->error());
    tab()->setSelectedTag("notes");
    auto *folder = qobject_cast<FolderModel *>(filesRoot()->property("folder").value<QObject *>());
    QVERIFY(folder); QTRY_VERIFY_WITH_TIMEOUT(!folder->loading(), asyncTimeout);
    QTRY_COMPARE_WITH_TIMEOUT(folder->rowCount(), 0, asyncTimeout);
    QFile file(marker); QVERIFY(file.open(QIODevice::WriteOnly)); file.close();
    const auto helper = deviceClientHelper(temporary,
        "print(json.dumps({'devices':[json.loads('" + deviceClientRow + "')]}),flush=True)\n"
        "for line in sys.stdin: pass\n");
    auto *devices = m_engine->singletonInstance<Devices *>("omafiles", "Devices");
    auto *process = devices->findChild<QProcess *>(); QVERIFY(process);
    if (process->state() != QProcess::NotRunning) { process->kill(); QTRY_COMPARE_WITH_TIMEOUT(process->state(), QProcess::NotRunning, asyncTimeout); }
    const auto stop = qScopeGuard([&] { process->kill(); process->waitForFinished(asyncTimeout); });
    key('d');
    auto *dock = window()->property("activeDock").value<QObject *>(); QVERIFY(dock);
    auto *heading = qobject_cast<QQuickItem *>(dock->property("devicesFocusTarget").value<QObject *>()); QVERIFY(heading);
    QTRY_VERIFY_WITH_TIMEOUT(heading->hasActiveFocus(), asyncTimeout);
    devices->start(helper);
    QTRY_COMPARE_WITH_TIMEOUT(devices->entries().size(), 1, asyncTimeout);
    auto *list = qobject_cast<QQuickItem *>(window()->property("activeDevicesList").value<QObject *>());
    QVERIFY(list); QTRY_VERIFY_WITH_TIMEOUT(list->hasActiveFocus(), asyncTimeout);
    QVERIFY(!heading->activeFocusOnTab());
    QTRY_VERIFY_WITH_TIMEOUT(folder->rowCount() > 0, asyncTimeout);
    QVERIFY(rootState(roots().first(), nullptr) == PathState::Present);
    QTRY_VERIFY_WITH_TIMEOUT(m_index->fileCount() > 0, asyncTimeout);
    QTRY_VERIFY_WITH_TIMEOUT(m_index->error().isEmpty(), asyncTimeout);
    QTRY_VERIFY_WITH_TIMEOUT(m_index->tagCount() > 0, asyncTimeout);
    QVERIFY(QFile::remove(marker));
    devices->storageChanged();
    QTRY_COMPARE_WITH_TIMEOUT(m_index->fileCount(), 0, asyncTimeout);
    QTRY_VERIFY_WITH_TIMEOUT(m_index->error().contains("root that is not here"), asyncTimeout);
    QTRY_COMPARE_WITH_TIMEOUT(m_index->tagCount(), 0, asyncTimeout);
    QTRY_COMPARE_WITH_TIMEOUT(folder->rowCount(), 0, asyncTimeout);
}


void TestStudio::refreshesStoragePresenceOffTheGuiThreadAndCancelsOldWork_data()
{
    QTest::addColumn<bool>("reloadIndex");
    QTest::newRow("storage-event") << false;
    QTest::newRow("index-refresh") << true;
}

void TestStudio::refreshesStoragePresenceOffTheGuiThreadAndCancelsOldWork()
{
    QFETCH(bool, reloadIndex);
    const QString marker = m_tree.filePath("device-available");
    QFile::remove(marker);
    QThreadPool *pool = QThreadPool::globalInstance();
    pool->waitForDone();
    const auto rootsBefore = declareRoots({Root{"test-device", m_tree.path(), true, "device-available"}});
    const auto restore = qScopeGuard([&] { pool->waitForDone(); declareRoots(rootsBefore); QFile::remove(marker); });
    IndexModel model;
    QTRY_VERIFY(!model.loading());
    QCOMPARE(model.fileCount(), 0);
    QVERIFY(model.error().contains("root that is not here"));
    const int threads = pool->maxThreadCount();
    pool->setMaxThreadCount(1);
    QSemaphore entered, release;
    const auto cleanup = qScopeGuard([&] {
        release.release();
        pool->waitForDone();
        pool->setMaxThreadCount(threads);
    });
    const auto blocker = QtConcurrent::run([&] { entered.release(); release.acquire(); });
    QVERIFY(entered.tryAcquire(1, asyncTimeout));
    QFile file(marker); QVERIFY(file.open(QIODevice::WriteOnly)); file.close();
    QSignalSpy published(&model, &IndexModel::changed);
    const auto refresh = [&] { if (reloadIndex) model.reload(); else model.refreshStorage(); };
    refresh();
    QCoreApplication::processEvents();
    QCOMPARE(model.fileCount(), 0);
    QCOMPARE(published.count(), 0);
    QVERIFY(QFile::remove(marker));
    refresh();
    release.release();
    pool->waitForDone();
    QTRY_COMPARE_WITH_TIMEOUT(published.count(), 1, asyncTimeout);
    QCOMPARE(model.fileCount(), 0);
    QVERIFY(model.error().contains("root that is not here"));
    QVERIFY(file.open(QIODevice::WriteOnly)); file.close();
    refresh();
    QTRY_VERIFY_WITH_TIMEOUT(model.fileCount() > 0, asyncTimeout);
    QVERIFY(model.error().isEmpty());
    QVERIFY(model.tagCount() > 0);
}


void TestStudio::navigatesPortalChooser_data()
{
    QTest::addColumn<QString>("method");
    QTest::addColumn<QString>("extra");
    QTest::addColumn<QString>("initial");
    QTest::newRow("open") << "OpenFile" << "'multiple': <true>" << "chooserFiles";
    QTest::newRow("nonmodal") << "OpenFile" << "'multiple': <true>, 'modal': <false>" << "chooserFiles";
    QTest::newRow("multiple-folders") << "OpenFile" << "'directory': <true>, 'multiple': <true>" << "chooserFiles";
    QTest::newRow("save") << "SaveFile" << "'current_name': <'plain.txt'>" << "chooserFilename";
    QTest::newRow("folders") << "OpenFile" << "'directory': <true>" << "chooserFiles";
    QTest::newRow("save-files") << "SaveFiles" << "'files': <@aay [[110,101,119,46,116,120,116,0], [110,101,119,46,116,120,116,0]]>" << "chooserFiles";
}

void TestStudio::navigatesPortalChooser()
{
    QFETCH(QString, method);
    QFETCH(QString, extra);
    QFETCH(QString, initial);
    QTemporaryDir runtime;
    QProcess daemon;
    auto environment = QProcessEnvironment::systemEnvironment();
    environment.insert("XDG_RUNTIME_DIR", runtime.path());
    daemon.setProcessEnvironment(environment);
    daemon.start("dbus-daemon", {"--session", "--nofork", "--print-address=1"});
    QVERIFY(daemon.waitForStarted(asyncTimeout));
    const auto stopDaemon = qScopeGuard([&] { daemon.terminate(); daemon.waitForFinished(asyncTimeout); });
    QVERIFY(daemon.bytesAvailable() || daemon.waitForReadyRead(asyncTimeout));
    const QString address = QString::fromUtf8(daemon.readLine()).trimmed();
    auto server = QDBusConnection::connectToBus(address, "chooser-ui");
    const auto disconnect = qScopeGuard([] { QDBusConnection::disconnectFromBus("chooser-ui"); });
    QQmlApplicationEngine engine;
    engine.load(QUrl(qEnvironmentVariable("OMAFILES_CHOOSER_QML", "qrc:/qml/FileChooser.qml")));
    QVERIFY(!engine.rootObjects().isEmpty());
    auto *portal = engine.singletonInstance<Portal *>("omafiles", "Portal");
    QVERIFY(portal->start(server));
    auto *chooser = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
    QVERIFY(chooser);
    const auto control = [chooser](const QString &name) -> QQuickItem * {
        QList<QQuickItem *> queue{chooser->contentItem()};
        while (!queue.isEmpty()) {
            auto *item = queue.takeFirst();
            if (item->objectName() == name && item->isVisible()) return item;
            queue.append(item->childItems());
        }
        return nullptr;
    };
    QProcess caller;
    caller.setProcessEnvironment(environment);
    QStringList folderBytes;
    for (const auto byte : m_tree.path().toUtf8() + '\0')
        folderBytes << QString::number(static_cast<unsigned char>(byte));
    const QString options = "{'current_folder': <@ay [" + folderBytes.join(",")
        + "]>, 'filters': <[('Text', [(uint32 0, '*.txt')]), ('All', [(uint32 0, '*')])]>, "
          "'choices': <[('read', 'Read only', [], 'false'), "
          "('encoding', 'Encoding', [('utf8', 'UTF-8'), ('latin1', 'Latin-1')], 'utf8')]>, " + extra + "}";
    caller.start("gdbus", {"call", "--timeout", "600", "--address", address, "--dest", "org.freedesktop.impl.portal.desktop.omafiles",
        "--object-path", "/org/freedesktop/portal/desktop", "--method", "org.freedesktop.impl.portal.FileChooser." + method,
        "/org/freedesktop/portal/desktop/request/ui/test", "org.test.Caller", "", "Choose a file", options});
    QVERIFY(caller.waitForStarted(asyncTimeout));
    const auto stopCaller = qScopeGuard([&] { if (caller.state() != QProcess::NotRunning) { caller.kill(); caller.waitForFinished(asyncTimeout); } });
    QTRY_VERIFY_WITH_TIMEOUT(portal->current() || caller.state() == QProcess::NotRunning, asyncTimeout);
    QVERIFY2(portal->current(), caller.readAllStandardError().constData());
    QVERIFY(QTest::qWaitForWindowExposed(chooser, asyncTimeout));
    QCOMPARE(chooser->modality(), portal->current()->modal ? Qt::WindowModal : Qt::NonModal);
    auto *folder = chooser->property("folderModel").value<FolderModel *>();
    QVERIFY(folder);
    QTRY_VERIFY_WITH_TIMEOUT(!folder->loading() && folder->rowCount() > 2, asyncTimeout);
    auto *first = control(initial);
    QVERIFY(first);
    auto *firstTarget = first->property("focusTarget").value<QQuickItem *>();
    QTRY_VERIFY_WITH_TIMEOUT((firstTarget ? firstTarget : first)->hasActiveFocus(), asyncTimeout);
    auto *startingFocus = chooser->activeFocusItem();
    QVERIFY(startingFocus);
    const auto focusName = [](QQuickItem *item) {
        while (item && item->objectName().isEmpty()) item = item->parentItem();
        return item ? item->objectName() : QString();
    };
    QStringList expected{initial, "chooserUp", "chooserHome", "chooserHidden", "chooserHelp", "chooserLocation", "chooserSearch"};
    if (method == "SaveFile") expected << "chooserFiles";
    expected << "chooserFilter" << "chooserBooleanChoice" << "chooserChoice";
    if (portal->current()->multiple) expected << "chooserMark";
    expected << "chooserCancel" << "chooserAccept";
    QStringList seen;
    for (int i = 0; i < expected.size(); ++i) {
        QVERIFY(chooser->activeFocusItem());
        seen << focusName(chooser->activeFocusItem());
        QTest::keyClick(chooser, Qt::Key_Tab);
    }
    QCOMPARE(seen, expected);
    QCOMPARE(chooser->activeFocusItem(), startingFocus);
    for (int i = expected.size() - 1; i >= 0; --i) {
        QTest::keyClick(chooser, Qt::Key_Backtab);
        QCOMPARE(focusName(chooser->activeFocusItem()), expected.at(i));
    }
    QTest::keyClick(chooser, Qt::Key_Tab, Qt::ShiftModifier);
    QCOMPARE(focusName(chooser->activeFocusItem()), QStringLiteral("chooserAccept"));
    QTest::keyClick(chooser, Qt::Key_Tab);
    QCOMPARE(focusName(chooser->activeFocusItem()), initial);
    auto *hidden = control("chooserHidden");
    QVERIFY(hidden);
    const QPoint hiddenPoint = hidden->mapToScene(QPointF(hidden->width() / 2, hidden->height() / 2)).toPoint();
    static auto *touch = QTest::createTouchDevice();
    auto sequence = QTest::touchEvent(chooser, touch, false);
    sequence.press(0, hiddenPoint, chooser).commit();
    sequence.release(0, hiddenPoint, chooser).commit();
    QTRY_VERIFY(folder->showHidden());
    QTest::mouseClick(chooser, Qt::LeftButton, Qt::NoModifier, hiddenPoint);
    QTRY_VERIFY(!folder->showHidden());
    chooser->resize(420, 300);
    QTRY_COMPARE(chooser->size(), QSize(420, 300));
    auto *scroll = control("chooserForm");
    QVERIFY(scroll);
    const auto paint = [chooser] {
        QSignalSpy frames(chooser, &QQuickWindow::frameSwapped);
        chooser->update();
        return frames.wait(asyncTimeout);
    };
    QPointer<QQuickItem> boolean = control("chooserBooleanChoice");
    QVERIFY(boolean);
    boolean->forceActiveFocus();
    QTRY_VERIFY_WITH_TIMEOUT(scroll->property("contentY").toReal() > 0, asyncTimeout);
    QTest::keyClick(chooser, Qt::Key_Space);
    QCOMPARE(portal->current()->choiceValues().value("read").toString(), QStringLiteral("true"));
    QVERIFY(paint());
    QVERIFY(boolean);
    QCOMPARE(control("chooserBooleanChoice"), boolean.data());
    QVERIFY(boolean->hasActiveFocus());
    QPointer<QQuickItem> dropdown = control("chooserChoice");
    dropdown->forceActiveFocus();
    QTest::keyClick(chooser, Qt::Key_Down);
    QCOMPARE(portal->current()->choiceValues().value("encoding").toString(), QStringLiteral("latin1"));
    QVERIFY(paint());
    QVERIFY(dropdown);
    QCOMPARE(control("chooserChoice"), dropdown.data());
    QVERIFY(dropdown->hasActiveFocus());
    QTest::keyClick(chooser, Qt::Key_Space);
    auto *choicePopup = dropdown->property("popup").value<QObject *>();
    QVERIFY(choicePopup);
    QTRY_VERIFY(choicePopup->property("visible").toBool());
    QTest::keyClick(chooser, Qt::Key_Escape);
    QTRY_VERIFY(!choicePopup->property("visible").toBool());
    QVERIFY(portal->current());
    QTest::keyClick(chooser, Qt::Key_Space);
    QTRY_VERIFY(choicePopup->property("visible").toBool());
    QTest::keyClick(chooser, Qt::Key_Tab);
    QTRY_VERIFY(!choicePopup->property("visible").toBool());
    QCOMPARE(focusName(chooser->activeFocusItem()), portal->current()->multiple
        ? QStringLiteral("chooserMark") : QStringLiteral("chooserCancel"));
    QTest::keyClick(chooser, Qt::Key_Backtab);
    QVERIFY(dropdown->hasActiveFocus());
    auto *filter = control("chooserFilter");
    QVERIFY(filter);
    filter->forceActiveFocus();
    QTest::keyClick(chooser, Qt::Key_Space);
    auto *filterPopup = filter->property("popup").value<QObject *>();
    QVERIFY(filterPopup);
    QTRY_VERIFY(filterPopup->property("visible").toBool());
    QTest::keyClick(chooser, Qt::Key_Down);
    QTest::keyClick(chooser, Qt::Key_Return);
    QTRY_VERIFY(portal->current() && portal->current()->selectedFilter() == 1);
    QTRY_VERIFY(!filterPopup->property("visible").toBool());
    auto *files = control("chooserFiles");
    QVERIFY(files);
    if (method == "SaveFile") {
        auto *name = control("chooserFilename");
        name->setProperty("text", "plain.txt");
        QMetaObject::invokeMethod(name, "focusEntry", Q_ARG(QVariant, QVariant(-1)));
        QTest::keyClick(chooser, Qt::Key_Return);
        QTRY_VERIFY_WITH_TIMEOUT(!portal->current()->overwritePaths.isEmpty(), asyncTimeout);
        QTRY_COMPARE(focusName(chooser->activeFocusItem()), QStringLiteral("chooserKeepEditing"));
        QTest::keyClick(chooser, Qt::Key_Tab);
        QTest::keyClick(chooser, Qt::Key_Space);
    } else if (method == "OpenFile" && !portal->current()->directory) {
        auto *search = control("chooserSearch");
        search->setProperty("text", "plain.txt");
        QTRY_VERIFY_WITH_TIMEOUT(!folder->loading() && folder->rowCount() == 2, asyncTimeout);
        files->forceActiveFocus();
        QTest::keyClick(chooser, Qt::Key_End);
        QTest::keyClick(chooser, Qt::Key_Space);
        QCOMPARE(folder->markedCount(), 1);
        auto *accept = control("chooserAccept");
        accept->forceActiveFocus();
        QTest::keyClick(chooser, Qt::Key_Return);
    } else if (portal->current()->multiple && portal->current()->directory) {
        files->forceActiveFocus();
        QTest::keyClick(chooser, Qt::Key_Home);
        QTest::keyClick(chooser, Qt::Key_Down);
        QTest::keyClick(chooser, Qt::Key_Space);
        QTest::keyClick(chooser, Qt::Key_Down);
        auto *mark = control("chooserMark");
        mark->forceActiveFocus();
        QTest::keyClick(chooser, Qt::Key_Space);
        QCOMPARE(folder->markedCount(), 2);
        auto *accept = control("chooserAccept");
        accept->forceActiveFocus();
        QTest::keyClick(chooser, Qt::Key_Return);
    } else {
        files->forceActiveFocus();
        QTest::keyClick(chooser, Qt::Key_Home);
        QTest::keyClick(chooser, Qt::Key_Return, Qt::ControlModifier);
    }
    QTRY_VERIFY_WITH_TIMEOUT(!portal->current(), asyncTimeout);
    QTRY_COMPARE_WITH_TIMEOUT(caller.state(), QProcess::NotRunning, asyncTimeout);
    QCOMPARE(caller.exitCode(), 0);
    const QByteArray reply = caller.readAllStandardOutput();
    QVERIFY2(reply.startsWith("(uint32 0,"), reply.constData());
    QVERIFY(reply.contains("file://"));
    QVERIFY(reply.contains("All"));
    QVERIFY(reply.contains("('read', 'true')"));
    QVERIFY(reply.contains("('encoding', 'latin1')"));
    QVERIFY(QFileInfo::exists(m_tree.filePath("plain.txt")));
    QVERIFY(!QFileInfo::exists(m_tree.filePath("new.txt")));
    for (const auto &entryName : {"chooserLocation", "chooserSearch", "chooserFilename"}) {
        if (QByteArray(entryName) == "chooserFilename" && method != "SaveFile") continue;
        caller.start("gdbus", caller.arguments());
        QVERIFY(caller.waitForStarted(asyncTimeout));
        QTRY_VERIFY_WITH_TIMEOUT(portal->current() || caller.state() == QProcess::NotRunning, asyncTimeout);
        QVERIFY2(portal->current(), caller.readAllStandardError().constData());
        auto *entry = control(entryName);
        QVERIFY(entry);
        QMetaObject::invokeMethod(entry, "focusEntry", Q_ARG(QVariant, QVariant(-1)));
        QTest::keyClick(chooser, Qt::Key_Escape);
        QTRY_VERIFY_WITH_TIMEOUT(!portal->current(), asyncTimeout);
        QTRY_COMPARE_WITH_TIMEOUT(caller.state(), QProcess::NotRunning, asyncTimeout);
        QCOMPARE(caller.exitCode(), 0);
        QVERIFY(caller.readAllStandardOutput().startsWith("(uint32 1,"));
    }
}

void TestStudio::expiresTrashReceiptsWithTheirState()
{
    QTemporaryDir fixture;
    QVERIFY(fixture.isValid());
    const QByteArray oldData = qgetenv("XDG_DATA_HOME");
    auto cleanup = qScopeGuard([&] { qputenv("XDG_DATA_HOME", oldData); });
    for (int change = 0; change < 8; ++change) {
        const QDir dir(fixture.path() + QStringLiteral("/case%1").arg(change));
        qputenv("XDG_DATA_HOME", dir.path().toUtf8());
        const auto add = [&](const QString &name, const QString &extra = QString()) {
            return writeLooknfeel(dir.filePath("Trash/files/") + name, "payload")
                && writeLooknfeel(dir.filePath("Trash/info/") + name + ".trashinfo",
                    "[Trash Info]\nPath=" + dir.filePath(name)
                    + "\nDeletionDate=2026-10-11T13:14:15\n" + extra);
        };
        QVERIFY(add("first"));
        QVERIFY(add("remaining"));
        TrashModel model;
        QTRY_VERIFY(!model.loading() && model.count() == 2);
        if (change == 6) {
            QVERIFY(model.prepareEmpty());
            model.empty();
        } else {
            if (change == 7) QVERIFY(writeLooknfeel(dir.filePath("first"), "occupied"));
            model.restore(model.rowForPath(dir.filePath("Trash/files/first")));
        }
        QTRY_VERIFY(!model.busy() && !model.loading());
        const QString receipt = model.message();
        QVERIFY2(receipt.startsWith(change == 6 ? "Permanently deleted 2"
                                   : change == 7 ? "Restored 0" : "Restored 1"), qPrintable(receipt));
        if (change == 7) QVERIFY(receipt.contains("already exists"));
        QCOMPARE(model.count(), change == 6 ? 0 : change == 7 ? 2 : 1);
        model.reload();
        QVERIFY(model.loading());
        QCOMPARE(model.message(), QStringLiteral("Refreshing Trash…"));
        QTRY_VERIFY(!model.loading());
        QCOMPARE(model.message(), receipt);

        const QString payload = dir.filePath("Trash/files/remaining");
        const QString info = dir.filePath("Trash/info/remaining.trashinfo");
        switch (change) {
        case 0: case 6:
            QVERIFY(add("arrived"));
            break;
        case 1:
            QVERIFY(QFile::remove(payload));
            QVERIFY(QFile::remove(info));
            break;
        case 2:
            // Only raw metadata changes; the visible row values stay the same.
            QVERIFY(writeLooknfeel(info, "[Trash Info]\nPath=" + dir.filePath("remaining")
                + "\nDeletionDate=2026-10-11T13:14:15\nExtra=changed\n"));
            break;
        case 3:
            QVERIFY(writeLooknfeel(payload, "payload changed"));
            break;
        case 4:
            QVERIFY(QFile::setPermissions(info, QFile::ReadOwner | QFile::WriteOwner | QFile::ReadGroup));
            break;
        case 5:
            // A scan error changes even though the item list remains identical.
            QVERIFY(writeLooknfeel(dir.filePath("Trash/info/unexpected"), "invalid entry"));
            break;
        case 7:
            QVERIFY(writeLooknfeel(info, "malformed metadata"));
            break;
        }
        model.reload();
        QTRY_VERIFY(!model.loading());
        if (change == 5) {
            QVERIFY(!model.error().isEmpty());
            QCOMPARE(model.message(), model.error());
            QVERIFY(QFile::remove(dir.filePath("Trash/info/unexpected")));
            model.reload();
            QTRY_VERIFY(!model.loading());
            QVERIFY(model.error().isEmpty());
        }
        QVERIFY2(model.message().isEmpty(), qPrintable(model.message()));
    }

    const QDir broken(fixture.path() + "/scan-error");
    qputenv("XDG_DATA_HOME", broken.path().toUtf8());
    QVERIFY(writeLooknfeel(broken.filePath("Trash/files/only"), "payload"));
    QVERIFY(writeLooknfeel(broken.filePath("Trash/info/only.trashinfo"),
        "[Trash Info]\nPath=" + broken.filePath("only") + "\nDeletionDate=2026-10-11T13:14:15\n"));
    QVERIFY(writeLooknfeel(broken.filePath("Trash/info/unexpected"), "invalid entry"));
    TrashModel model;
    QTRY_VERIFY(!model.loading() && model.count() == 1);
    QVERIFY(!model.error().isEmpty());
    model.restore(0);
    QTRY_VERIFY(!model.busy() && !model.loading());
    QCOMPARE(model.count(), 0);
    QVERIFY(!model.error().isEmpty());
    QCOMPARE(model.message(), model.error());
    QVERIFY(QFile::remove(broken.filePath("Trash/info/unexpected")));
    model.reload();
    QTRY_VERIFY(!model.loading());
    QVERIFY(model.error().isEmpty());
    QVERIFY(model.message().isEmpty());
}

void TestStudio::expiresReceiptsWhenIndexStateChanges()
{
    QTemporaryDir fixture;
    QVERIFY(fixture.isValid());
    const QByteArray previousData = qgetenv("XDG_DATA_HOME");
    qputenv("XDG_DATA_HOME", fixture.filePath("data").toUtf8());
    const auto restore = qScopeGuard([&] { qputenv("XDG_DATA_HOME", previousData); });
    const QString file = fixture.filePath("document.txt");
    QVERIFY(writeLooknfeel(file, "data"));
    IndexModel model;
    QTRY_VERIFY(!model.loading());
    model.setNotice("Finished operation");
    model.reload();
    QTRY_VERIFY(!model.loading());
    QCOMPARE(model.notice(), QString("Finished operation"));
    QString error;
    {
        Store store(defaultIndexPath());
        QVERIFY(store.lockAndLoad(&error));
        Record record; record.path = file; record.tags = {"first"};
        store.put(record);
        QVERIFY(store.commit(&error));
    }
    model.reload();
    QTRY_VERIFY(!model.loading());
    QVERIFY(model.notice().isEmpty());
    model.setNotice("Finished operation");
    {
        Store store(defaultIndexPath());
        QVERIFY(store.lockAndLoad(&error));
        auto record = store.get(file); record.tags = {"second"};
        store.put(record);
        QVERIFY(store.commit(&error));
    }
    model.reload();
    QTRY_VERIFY(!model.loading());
    QVERIFY(model.notice().isEmpty());
    model.setNotice("Finished operation");
    QVERIFY(QFile::remove(file));
    model.reload();
    QTRY_VERIFY(!model.loading());
    QVERIFY(model.notice().isEmpty());
    QVERIFY(!model.error().isEmpty());
    model.setNotice("Finished operation");
    model.refreshStorage();
    QVERIFY(model.notice().isEmpty());
    QThreadPool::globalInstance()->waitForDone();
}

void TestStudio::prioritizesCurrentStatusAndExpiresDeviceReceipts()
{
    QTemporaryDir fixture;
    QVERIFY(fixture.isValid());
    const auto helper = deviceClientHelper(fixture,
        "row=json.loads('" + deviceClientRow + "')\n"
        "print(json.dumps({'devices':[row]}),flush=True)\n"
        "count=0; pending=None\n"
        "for line in sys.stdin:\n"
        " c=json.loads(line)\n"
        " if c['action']=='refresh': print(json.dumps({'devices':[]}),flush=True)\n"
        " elif c['action']=='cancel':\n"
        "  print(json.dumps(dict(pending,finished=True,ok=False,reason='Canceled')),flush=True)\n"
        " else:\n"
        "  count+=1\n"
        "  if count==1: print(json.dumps(dict(c,finished=True,ok=True)),flush=True)\n"
        "  else: pending=c\n");
    auto *devices = m_engine->singletonInstance<Devices *>("omafiles", "Devices");
    auto *process = devices->findChild<QProcess *>(); QVERIFY(process);
    if (process->state() != QProcess::NotRunning) {
        process->kill();
        QTRY_COMPARE(process->state(), QProcess::NotRunning);
    }
    const auto stop = qScopeGuard([&] { process->kill(); process->waitForFinished(asyncTimeout); });
    devices->start(helper);
    QTRY_COMPARE(devices->entries().size(), 1);
    auto *status = itemsNamed("statusBar").first();
    m_index->reload();
    settle();
    devices->act("usb:test", "mount");
    QTRY_VERIFY(!devices->busy());
    QTRY_COMPARE(m_index->notice(), QString("Device operation completed"));
    QCOMPARE(status->property("message").toString(), m_index->notice());
    devices->act("usb:test", "mount");
    QVERIFY(devices->busy());
    QVERIFY(m_index->notice().isEmpty());
    QCOMPARE(status->property("message").toString(), QString("Device operation in progress"));
    devices->act("usb:test", "cancel");
    QCOMPARE(status->property("message").toString(), QString("Canceling device operation"));
    QTRY_VERIFY(!devices->busy());
    QTRY_COMPARE(m_index->notice(), QString("Canceled"));
    devices->refresh();
    QTRY_VERIFY(devices->entries().isEmpty());
    QVERIFY(m_index->notice().isEmpty());
}

void TestStudio::preservesNewReceiptsDuringStorageRefresh()
{
    QTemporaryDir fixture;
    QVERIFY(fixture.isValid());
    const auto oldRoots = roots();
    const auto restore = qScopeGuard([&] { declareRoots(oldRoots); m_index->reload(); });
    declareRoots({Root{"usb", fixture.filePath("usb"), false, "marker"}});
    m_index->reload();
    settle();
    QVERIFY(m_index->error().contains("root usb is declared and not here"));
    QVERIFY(writeLooknfeel(fixture.filePath("usb/marker"), "present"));
    QThreadPool *pool = QThreadPool::globalInstance();
    pool->waitForDone();
    const int threads = pool->maxThreadCount();
    pool->setMaxThreadCount(1);
    QSemaphore entered, release;
    const auto cleanup = qScopeGuard([&] {
        release.release();
        pool->waitForDone();
        pool->setMaxThreadCount(threads);
    });
    const auto blocker = QtConcurrent::run([&] { entered.release(); release.acquire(); });
    QVERIFY(entered.tryAcquire(1, asyncTimeout));
    m_index->reload();
    QVERIFY(m_index->loading());
    m_index->setNotice("New completed operation");
    release.release();
    QTRY_VERIFY(!m_index->loading());
    QVERIFY(m_index->error().isEmpty());
    QCOMPARE(m_index->notice(), QString("New completed operation"));
}

void TestStudio::invalidatesReceiptsAcrossContextsAndActiveOperations()
{
    QTemporaryDir fixture;
    QVERIFY(fixture.isValid());
    auto *status = itemsNamed("statusBar").first();
    m_index->setNotice("Finished operation");
    tab()->setCwd(fixture.path());
    settle();
    QVERIFY(m_index->notice().isEmpty());
    m_index->setNotice("Finished operation");
    m_workspace->openTabIn(0, "files", m_tree.path());
    settle();
    QVERIFY(m_index->notice().isEmpty());

    tab()->setCwd(fixture.path());
    settle();
    QVERIFY(m_index->makeFolder(fixture.path(), "own-folder"));
    auto *folder = qobject_cast<FolderModel *>(filesRoot()->property("folder").value<QObject *>());
    QVERIFY(folder);
    QTRY_VERIFY(!folder->loading() && folder->rowForPath(fixture.filePath("own-folder")) >= 0);
    QCOMPARE(m_index->notice(), QString("made own-folder"));
    m_index->setNotice("Finished operation");
    key('j');
    QCOMPARE(m_index->notice(), QString("Finished operation"));
    QVERIFY(writeLooknfeel(fixture.filePath("external.txt"), "external change"));
    QTRY_VERIFY(!folder->loading() && folder->rowForPath(fixture.filePath("external.txt")) >= 0);
    QVERIFY(m_index->notice().isEmpty());

    VerbReceipt pending;
    const auto runner = setVerbRunner([&](const QStringList &, VerbReceipt done) { pending = std::move(done); });
    const auto restore = qScopeGuard([&] { setVerbRunner(runner); });
    m_index->verifyTree(m_tree.path());
    QVERIFY(pending);
    tab()->setCwd(m_tree.path());
    settle();
    pending(true, {}, {});
    QVERIFY(m_index->notice().isEmpty());
    m_index->verifyTree(m_tree.path());
    auto older = std::move(pending);
    m_index->verifyTree(m_tree.path());
    older(false, {}, "Older result");
    QVERIFY(m_index->notice().isEmpty());
    pending(false, {}, "Newer result");
    QCOMPARE(m_index->notice(), QString("Newer result"));
    m_index->verifyTree(m_tree.path());
    m_index->setNotice("New synchronous result");
    pending(false, {}, "Older asynchronous result");
    QCOMPARE(m_index->notice(), QString("New synchronous result"));
    pending = {};
    m_index->setNotice("Finished operation");
    const QString file = fixture.filePath("document.txt");
    QVERIFY(writeLooknfeel(file, "data"));
    m_index->tagAdd({file}, "work");
    QTRY_VERIFY(bool(pending));
    QVERIFY(m_index->operationBusy());
    QVERIFY(m_index->notice().isEmpty());
    QCOMPARE(status->property("message").toString(), QString("File operation in progress"));
    pending(false, {}, "Stopped by test");
    QTRY_VERIFY(!m_index->operationBusy());
    QTRY_COMPARE(m_index->notice(), QString("Stopped by test"));

    JournalDriver driver;
    m_index->compress({file}, fixture.filePath("documents.tar.gz"), "tar.gz");
    QVERIFY(m_index->archive()->busy());
    QVERIFY(m_index->notice().isEmpty());
    QCOMPARE(status->property("message").toString(), m_index->archive()->progress());
    m_index->archive()->updateProgress("Writing archive entries");
    QCOMPARE(status->property("message").toString(), QString("Writing archive entries"));
    tab()->setCwd(fixture.path());
    settle();
    m_index->cancelArchive();
    if (m_index->archive()->busy())
        QCOMPARE(status->property("message").toString(), m_index->archive()->progress());
    QTRY_VERIFY(!m_index->archive()->busy());
    QVERIFY(m_index->notice().isEmpty());
}

void TestStudio::publishesReceiptsAfterUpdatingOperationState()
{
    QTemporaryDir fixture;
    QVERIFY(fixture.isValid());
    QVERIFY(QDir(fixture.path()).mkdir("out"));
    const QString source = fixture.filePath("source.txt");
    const QString copied = fixture.filePath("out/source.txt");
    QVERIFY(writeLooknfeel(source, "payload"));
    JournalDriver driver;
    m_index->tagAdd({source}, "receipt");
    QTRY_VERIFY(!m_index->tagEditing() && !m_index->operationBusy() && !m_index->loading());
    QCOMPARE(m_index->tagsForPath(source), QStringList{"receipt"});
    QVERIFY2(m_index->notice().contains("tagged 1 file"), qPrintable(m_index->notice()));
    m_index->copyInto({source}, fixture.filePath("out"));
    QTRY_VERIFY(!m_index->operationBusy() && !m_index->loading());
    QCOMPARE(m_index->tagsForPath(copied), QStringList{"receipt"});
    QVERIFY2(m_index->notice().contains("copied 1 file"), qPrintable(m_index->notice()));
    m_index->undo();
    QTRY_VERIFY(!m_index->operationBusy() && !m_index->loading());
    QVERIFY(!QFileInfo::exists(copied));
    QVERIFY2(!m_index->notice().isEmpty(), "Undo must publish its completed receipt after reloading the index");
    const QString warning = m_index->error();
    const auto launcher = setLauncher([](const OpenRequest &, QString *error) {
        *error = "Desktop action refused";
        return false;
    });
    const auto restore = qScopeGuard([&] { setLauncher(launcher); });
    for (bool share : {false, true}) {
        if (share) m_index->sharePath(source);
        else m_index->openPath(source);
        QCOMPARE(m_index->notice(), QString("Desktop action refused"));
        QCOMPARE(m_index->error(), warning);
        m_index->clearNotice();
        QCOMPARE(m_index->error(), warning);
    }
}
