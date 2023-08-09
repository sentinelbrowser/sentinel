/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=4 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "nsDragService.h"
#include "nsArrayUtils.h"
#include "nsIObserverService.h"
#include "nsWidgetsCID.h"
#include "nsWindow.h"
#include "nsSystemInfo.h"
#include "nsXPCOM.h"
#include "nsICookieJarSettings.h"
#include "nsISupportsPrimitives.h"
#include "nsIIOService.h"
#include "nsIFileURL.h"
#include "nsNetUtil.h"
#include "mozilla/Logging.h"
#include "nsTArray.h"
#include "nsPrimitiveHelpers.h"
#include "prtime.h"
#include "prthread.h"
#include <dlfcn.h>
#include <gtk/gtk.h>
#include "nsCRT.h"
#include "mozilla/BasicEvents.h"
#include "mozilla/Services.h"
#include "mozilla/ClearOnShutdown.h"
#include "mozilla/PresShell.h"
#include "mozilla/ScopeExit.h"
#include "mozilla/AutoRestore.h"
#include "mozilla/WidgetUtilsGtk.h"
#include "GRefPtr.h"
#include "nsAppShell.h"

#ifdef MOZ_X11
#  include "gfxXlibSurface.h"
#endif
#include "gfxContext.h"
#include "nsImageToPixbuf.h"
#include "nsPresContext.h"
#include "nsIContent.h"
#include "mozilla/dom/Document.h"
#include "nsViewManager.h"
#include "nsIFrame.h"
#include "nsGtkUtils.h"
#include "nsGtkKeyUtils.h"
#include "mozilla/gfx/2D.h"
#include "gfxPlatform.h"
#include "ScreenHelperGTK.h"
#include "nsArrayUtils.h"
#include "nsStringStream.h"
#include "nsDirectoryService.h"
#include "nsDirectoryServiceDefs.h"
#include "nsEscape.h"
#include "nsString.h"

using namespace mozilla;
using namespace mozilla::gfx;

//  The maximum time to wait for a "drag_received" arrived in microseconds.
#define NS_DND_TIMEOUT 1000000

//  The maximum time to wait before temporary files resulting
//  from drag'n'drop events will be removed in miliseconds.
//  It's set to 5 min as the file has to be present some time after drop
//  event to give target application time to get the data.
//  (A target application can throw a dialog to ask user what to do with
//  the data and will access the tmp file after user action.)
#define NS_DND_TMP_CLEANUP_TIMEOUT (1000 * 60 * 5)

#define NS_SYSTEMINFO_CONTRACTID "@mozilla.org/system-info;1"

// This sets how opaque the drag image is
#define DRAG_IMAGE_ALPHA_LEVEL 0.5

#ifdef MOZ_LOGGING
extern mozilla::LazyLogModule gWidgetDragLog;
#  define LOGDRAGSERVICE(str, ...)                    \
    MOZ_LOG(gWidgetDragLog, mozilla::LogLevel::Debug, \
            ("[Depth %d]: " str, GetLoopDepth(), ##__VA_ARGS__))
#  define LOGDRAGSERVICESTATIC(str, ...) \
    MOZ_LOG(gWidgetDragLog, mozilla::LogLevel::Debug, (str, ##__VA_ARGS__))
#else
#  define LOGDRAGSERVICE(...)
#endif

// Helper class to block native events processing.
class MOZ_STACK_CLASS AutoSuspendNativeEvents {
 public:
  AutoSuspendNativeEvents() {
    mAppShell = do_GetService(NS_APPSHELL_CID);
    mAppShell->SuspendNative();
  }
  ~AutoSuspendNativeEvents() { mAppShell->ResumeNative(); }

 private:
  nsCOMPtr<nsIAppShell> mAppShell;
};

// data used for synthetic periodic motion events sent to the source widget
// grabbing real events for the drag.
static guint sMotionEventTimerID;

static GdkEvent* sMotionEvent;
static GUniquePtr<GdkEvent> TakeMotionEvent() {
  GUniquePtr<GdkEvent> event(sMotionEvent);
  sMotionEvent = nullptr;
  return event;
}
static void SetMotionEvent(GUniquePtr<GdkEvent> aEvent) {
  TakeMotionEvent();
  sMotionEvent = aEvent.release();
}

static GtkWidget* sGrabWidget;

static constexpr nsLiteralString kDisallowedExportedSchemes[] = {
    u"about"_ns,      u"blob"_ns,     u"chrome"_ns,      u"imap"_ns,
    u"javascript"_ns, u"mailbox"_ns,  u"moz-anno"_ns,    u"news"_ns,
    u"page-icon"_ns,  u"resource"_ns, u"view-source"_ns, u"moz-extension"_ns,
};

// _NETSCAPE_URL is similar to text/uri-list type.
// Format is UTF8: URL + "\n" + title.
// While text/uri-list tells target application to fetch, copy and store data
// from URL, _NETSCAPE_URL suggest to create a link to the target.
// Also _NETSCAPE_URL points to only one item while text/uri-list can point to
// multiple ones.
static const char gMozUrlType[] = "_NETSCAPE_URL";
static const char gMimeListType[] = "application/x-moz-internal-item-list";
static const char gTextUriListType[] = "text/uri-list";
static const char gTextPlainUTF8Type[] = "text/plain;charset=utf-8";
static const char gXdndDirectSaveType[] = "XdndDirectSave0";
static const char gTabDropType[] = "application/x-moz-tabbrowser-tab";

// See https://docs.gtk.org/gtk3/enum.DragResult.html
static const char kGtkDragResults[][100]{
    "GTK_DRAG_RESULT_SUCCESS",        "GTK_DRAG_RESULT_NO_TARGET",
    "GTK_DRAG_RESULT_USER_CANCELLED", "GTK_DRAG_RESULT_TIMEOUT_EXPIRED",
    "GTK_DRAG_RESULT_GRAB_BROKEN",    "GTK_DRAG_RESULT_ERROR"};

static void invisibleSourceDragBegin(GtkWidget* aWidget,
                                     GdkDragContext* aContext, gpointer aData);

static void invisibleSourceDragEnd(GtkWidget* aWidget, GdkDragContext* aContext,
                                   gpointer aData);

static gboolean invisibleSourceDragFailed(GtkWidget* aWidget,
                                          GdkDragContext* aContext,
                                          gint aResult, gpointer aData);

static void invisibleSourceDragDataGet(GtkWidget* aWidget,
                                       GdkDragContext* aContext,
                                       GtkSelectionData* aSelectionData,
                                       guint aInfo, guint32 aTime,
                                       gpointer aData);

nsDragService::nsDragService()
    : mScheduledTask(eDragTaskNone),
      mTaskSource(0),
      mScheduledTaskIsRunning(false),
      mCachedDragContext() {
  // We have to destroy the hidden widget before the event loop stops
  // running.
  nsCOMPtr<nsIObserverService> obsServ =
      mozilla::services::GetObserverService();
  obsServ->AddObserver(this, "quit-application", false);

  // our hidden source widget
  // Using an offscreen window works around bug 983843.
  mHiddenWidget = gtk_offscreen_window_new();
  // make sure that the widget is realized so that
  // we can use it as a drag source.
  gtk_widget_realize(mHiddenWidget);
  // hook up our internal signals so that we can get some feedback
  // from our drag source
  g_signal_connect(mHiddenWidget, "drag_begin",
                   G_CALLBACK(invisibleSourceDragBegin), this);
  g_signal_connect(mHiddenWidget, "drag_data_get",
                   G_CALLBACK(invisibleSourceDragDataGet), this);
  g_signal_connect(mHiddenWidget, "drag_end",
                   G_CALLBACK(invisibleSourceDragEnd), this);
  // drag-failed is available from GTK+ version 2.12
  guint dragFailedID =
      g_signal_lookup("drag-failed", G_TYPE_FROM_INSTANCE(mHiddenWidget));
  if (dragFailedID) {
    g_signal_connect_closure_by_id(
        mHiddenWidget, dragFailedID, 0,
        g_cclosure_new(G_CALLBACK(invisibleSourceDragFailed), this, nullptr),
        FALSE);
  }

  // set up our logging module
  mCanDrop = false;
  mTargetDragDataReceived = false;
  mTargetDragData = 0;
  mTargetDragDataLen = 0;
  mTempFileTimerID = 0;
  mEventLoopDepth = 0;
  LOGDRAGSERVICE("nsDragService::nsDragService");
}

nsDragService::~nsDragService() {
  LOGDRAGSERVICE("nsDragService::~nsDragService");
  if (mTaskSource) g_source_remove(mTaskSource);
  if (mTempFileTimerID) {
    g_source_remove(mTempFileTimerID);
    RemoveTempFiles();
  }
}

NS_IMPL_ISUPPORTS_INHERITED(nsDragService, nsBaseDragService, nsIObserver)

mozilla::StaticRefPtr<nsDragService> sDragServiceInstance;
/* static */
already_AddRefed<nsDragService> nsDragService::GetInstance() {
  if (gfxPlatform::IsHeadless()) {
    return nullptr;
  }
  if (!sDragServiceInstance) {
    sDragServiceInstance = new nsDragService();
    ClearOnShutdown(&sDragServiceInstance);
  }

  RefPtr<nsDragService> service = sDragServiceInstance.get();
  return service.forget();
}

// nsIObserver

NS_IMETHODIMP
nsDragService::Observe(nsISupports* aSubject, const char* aTopic,
                       const char16_t* aData) {
  if (!nsCRT::strcmp(aTopic, "quit-application")) {
    LOGDRAGSERVICE("nsDragService::Observe(\"quit-application\")");
    if (mHiddenWidget) {
      gtk_widget_destroy(mHiddenWidget);
      mHiddenWidget = 0;
    }
    TargetResetData();
  } else {
    MOZ_ASSERT_UNREACHABLE("unexpected topic");
    return NS_ERROR_UNEXPECTED;
  }

  return NS_OK;
}

// Support for periodic drag events

// http://www.whatwg.org/specs/web-apps/current-work/multipage/dnd.html#drag-and-drop-processing-model
// and the Xdnd protocol both recommend that drag events are sent periodically,
// but GTK does not normally provide this.
//
// Here GTK is periodically stimulated by copies of the most recent mouse
// motion events so as to send drag position messages to the destination when
// appropriate (after it has received a status event from the previous
// message).
//
// (If events were sent only on the destination side then the destination
// would have no message to which it could reply with a drag status.  Without
// sending a drag status to the source, the destination would not be able to
// change its feedback re whether it could accept the drop, and so the
// source's behavior on drop will not be consistent.)

static gboolean DispatchMotionEventCopy(gpointer aData) {
  // Clear the timer id before OnSourceGrabEventAfter is called during event
  // dispatch.
  sMotionEventTimerID = 0;

  GUniquePtr<GdkEvent> event = TakeMotionEvent();
  // If there is no longer a grab on the widget, then the drag is over and
  // there is no need to continue drag motion.
  if (gtk_widget_has_grab(sGrabWidget)) {
    gtk_propagate_event(sGrabWidget, event.get());
  }

  // Cancel this timer;
  // We've already started another if the motion event was dispatched.
  return FALSE;
}

static void OnSourceGrabEventAfter(GtkWidget* widget, GdkEvent* event,
                                   gpointer user_data) {
  // If there is no longer a grab on the widget, then the drag motion is
  // over (though the data may not be fetched yet).
  if (!gtk_widget_has_grab(sGrabWidget)) return;

  if (event->type == GDK_MOTION_NOTIFY) {
    SetMotionEvent(GUniquePtr<GdkEvent>(gdk_event_copy(event)));

    // Update the cursor position.  The last of these recorded gets used for
    // the eDragEnd event.
    nsDragService* dragService = static_cast<nsDragService*>(user_data);
    gint scale = mozilla::widget::ScreenHelperGTK::GetGTKMonitorScaleFactor();
    auto p = LayoutDeviceIntPoint::Round(event->motion.x_root * scale,
                                         event->motion.y_root * scale);
    dragService->SetDragEndPoint(p);
  } else if (sMotionEvent &&
             (event->type == GDK_KEY_PRESS || event->type == GDK_KEY_RELEASE)) {
    // Update modifier state from key events.
    sMotionEvent->motion.state = event->key.state;
  } else {
    return;
  }

  if (sMotionEventTimerID) {
    g_source_remove(sMotionEventTimerID);
  }

  // G_PRIORITY_DEFAULT_IDLE is lower priority than GDK's redraw idle source
  // and lower than GTK's idle source that sends drag position messages after
  // motion-notify signals.
  //
  // http://www.whatwg.org/specs/web-apps/current-work/multipage/dnd.html#drag-and-drop-processing-model
  // recommends an interval of 350ms +/- 200ms.
  sMotionEventTimerID = g_timeout_add_full(
      G_PRIORITY_DEFAULT_IDLE, 350, DispatchMotionEventCopy, nullptr, nullptr);
}

static GtkWindow* GetGtkWindow(dom::Document* aDocument) {
  if (!aDocument) return nullptr;

  PresShell* presShell = aDocument->GetPresShell();
  if (!presShell) {
    return nullptr;
  }

  RefPtr<nsViewManager> vm = presShell->GetViewManager();
  if (!vm) return nullptr;

  nsCOMPtr<nsIWidget> widget = vm->GetRootWidget();
  if (!widget) return nullptr;

  GtkWidget* gtkWidget = static_cast<nsWindow*>(widget.get())->GetGtkWidget();
  if (!gtkWidget) return nullptr;

  GtkWidget* toplevel = nullptr;
  toplevel = gtk_widget_get_toplevel(gtkWidget);
  if (!GTK_IS_WINDOW(toplevel)) return nullptr;

  return GTK_WINDOW(toplevel);
}

// nsIDragService

NS_IMETHODIMP
nsDragService::InvokeDragSession(
    nsINode* aDOMNode, nsIPrincipal* aPrincipal, nsIContentSecurityPolicy* aCsp,
    nsICookieJarSettings* aCookieJarSettings, nsIArray* aArrayTransferables,
    uint32_t aActionType,
    nsContentPolicyType aContentPolicyType = nsIContentPolicy::TYPE_OTHER) {
  LOGDRAGSERVICE("nsDragService::InvokeDragSession");

  // If the previous source drag has not yet completed, signal handlers need
  // to be removed from sGrabWidget and dragend needs to be dispatched to
  // the source node, but we can't call EndDragSession yet because we don't
  // know whether or not the drag succeeded.
  if (mSourceNode) return NS_ERROR_NOT_AVAILABLE;

  return nsBaseDragService::InvokeDragSession(
      aDOMNode, aPrincipal, aCsp, aCookieJarSettings, aArrayTransferables,
      aActionType, aContentPolicyType);
}

// nsBaseDragService
nsresult nsDragService::InvokeDragSessionImpl(
    nsIArray* aArrayTransferables, const Maybe<CSSIntRegion>& aRegion,
    uint32_t aActionType) {
  // make sure that we have an array of transferables to use
  if (!aArrayTransferables) return NS_ERROR_INVALID_ARG;
  // set our reference to the transferables.  this will also addref
  // the transferables since we're going to hang onto this beyond the
  // length of this call
  mSourceDataItems = aArrayTransferables;

  LOGDRAGSERVICE("nsDragService::InvokeDragSessionImpl");

  // get the list of items we offer for drags
  GtkTargetList* sourceList = GetSourceList();

  if (!sourceList) return NS_OK;

  // save our action type
  GdkDragAction action = GDK_ACTION_DEFAULT;

  if (aActionType & DRAGDROP_ACTION_COPY)
    action = (GdkDragAction)(action | GDK_ACTION_COPY);
  if (aActionType & DRAGDROP_ACTION_MOVE)
    action = (GdkDragAction)(action | GDK_ACTION_MOVE);
  if (aActionType & DRAGDROP_ACTION_LINK)
    action = (GdkDragAction)(action | GDK_ACTION_LINK);

  GdkEvent* existingEvent = widget::GetLastMousePressEvent();
  GdkEvent fakeEvent;
  if (!existingEvent) {
    // Create a fake event for the drag so we can pass the time (so to speak).
    // If we don't do this, then, when the timestamp for the pending button
    // release event is used for the ungrab, the ungrab can fail due to the
    // timestamp being _earlier_ than CurrentTime.
    memset(&fakeEvent, 0, sizeof(GdkEvent));
    fakeEvent.type = GDK_BUTTON_PRESS;
    fakeEvent.button.window = gtk_widget_get_window(mHiddenWidget);
    fakeEvent.button.time = nsWindow::GetLastUserInputTime();
    fakeEvent.button.device = widget::GdkGetPointer();
  }

  // Put the drag widget in the window group of the source node so that the
  // gtk_grab_add during gtk_drag_begin is effective.
  // gtk_window_get_group(nullptr) returns the default window group.
  GtkWindowGroup* window_group =
      gtk_window_get_group(GetGtkWindow(mSourceDocument));
  gtk_window_group_add_window(window_group, GTK_WINDOW(mHiddenWidget));

  // start our drag.
  GdkDragContext* context = gtk_drag_begin_with_coordinates(
      mHiddenWidget, sourceList, action, 1,
      existingEvent ? existingEvent : &fakeEvent, -1, -1);

  if (widget::GdkIsWaylandDisplay() || widget::IsXWaylandProtocol()) {
    GdkDevice* device = gdk_drag_context_get_device(context);
    GdkWindow* gdkWindow =
        gdk_device_get_window_at_position(device, nullptr, nullptr);
    mSourceWindow = nsWindow::GetWindow(gdkWindow);
    if (mSourceWindow) {
      mSourceWindow->SetDragSource(context);
    }
  }

  LOGDRAGSERVICE("  GdkDragContext [%p] nsWindow [%p]", context,
                 mSourceWindow.get());

  nsresult rv;
  if (context) {
    StartDragSession();

    // GTK uses another hidden window for receiving mouse events.
    sGrabWidget = gtk_window_group_get_current_grab(window_group);
    if (sGrabWidget) {
      g_object_ref(sGrabWidget);
      // Only motion and key events are required but connect to
      // "event-after" as this is never blocked by other handlers.
      g_signal_connect(sGrabWidget, "event-after",
                       G_CALLBACK(OnSourceGrabEventAfter), this);
    }
    // We don't have a drag end point yet.
    mEndDragPoint = LayoutDeviceIntPoint(-1, -1);
    rv = NS_OK;
  } else {
    rv = NS_ERROR_FAILURE;
  }

  gtk_target_list_unref(sourceList);

  return rv;
}

bool nsDragService::SetAlphaPixmap(SourceSurface* aSurface,
                                   GdkDragContext* aContext, int32_t aXOffset,
                                   int32_t aYOffset,
                                   const LayoutDeviceIntRect& dragRect) {
  GdkScreen* screen = gtk_widget_get_screen(mHiddenWidget);

  // Transparent drag icons need, like a lot of transparency-related things,
  // a compositing X window manager
  if (!gdk_screen_is_composited(screen)) {
    return false;
  }

#ifdef cairo_image_surface_create
#  error "Looks like we're including Mozilla's cairo instead of system cairo"
#endif

  // TODO: grab X11 pixmap or image data instead of expensive readback.
  cairo_surface_t* surf = cairo_image_surface_create(
      CAIRO_FORMAT_ARGB32, dragRect.width, dragRect.height);
  if (!surf) return false;

  RefPtr<DrawTarget> dt = gfxPlatform::CreateDrawTargetForData(
      cairo_image_surface_get_data(surf),
      nsIntSize(dragRect.width, dragRect.height),
      cairo_image_surface_get_stride(surf), SurfaceFormat::B8G8R8A8);
  if (!dt) return false;

  dt->ClearRect(Rect(0, 0, dragRect.width, dragRect.height));
  dt->DrawSurface(
      aSurface, Rect(0, 0, dragRect.width, dragRect.height),
      Rect(0, 0, dragRect.width, dragRect.height), DrawSurfaceOptions(),
      DrawOptions(DRAG_IMAGE_ALPHA_LEVEL, CompositionOp::OP_SOURCE));

  cairo_surface_mark_dirty(surf);
  cairo_surface_set_device_offset(surf, -aXOffset, -aYOffset);

  // Ensure that the surface is drawn at the correct scale on HiDPI displays.
  static auto sCairoSurfaceSetDeviceScalePtr =
      (void (*)(cairo_surface_t*, double, double))dlsym(
          RTLD_DEFAULT, "cairo_surface_set_device_scale");
  if (sCairoSurfaceSetDeviceScalePtr) {
    gint scale = mozilla::widget::ScreenHelperGTK::GetGTKMonitorScaleFactor();
    sCairoSurfaceSetDeviceScalePtr(surf, scale, scale);
  }

  gtk_drag_set_icon_surface(aContext, surf);
  cairo_surface_destroy(surf);
  return true;
}

NS_IMETHODIMP
nsDragService::StartDragSession() {
  LOGDRAGSERVICE("nsDragService::StartDragSession");
  mTempFileUrls.Clear();
  return nsBaseDragService::StartDragSession();
}

bool nsDragService::RemoveTempFiles() {
  LOGDRAGSERVICE("nsDragService::RemoveTempFiles");

  // We can not delete the temporary files immediately after the
  // drag has finished, because the target application might have not
  // copied the temporary file yet. The Qt toolkit does not provide a
  // way to mark a drop as finished in an asynchronous way, so most
  // Qt based applications do send the dnd_finished signal before they
  // have actually accessed the data from the temporary file.
  // (https://bugreports.qt.io/browse/QTBUG-5441)
  //
  // To work also with these applications we collect all temporary
  // files in mTemporaryFiles array and remove them here in the timer event.
  auto files = std::move(mTemporaryFiles);
  for (nsIFile* file : files) {
#ifdef MOZ_LOGGING
    if (MOZ_LOG_TEST(gWidgetDragLog, LogLevel::Debug)) {
      nsAutoCString path;
      if (NS_SUCCEEDED(file->GetNativePath(path))) {
        LOGDRAGSERVICE("  removing %s", path.get());
      }
    }
#endif
    file->Remove(/* recursive = */ true);
  }
  MOZ_ASSERT(mTemporaryFiles.IsEmpty());
  mTempFileTimerID = 0;
  // Return false to remove the timer added by g_timeout_add_full().
  return false;
}

gboolean nsDragService::TaskRemoveTempFiles(gpointer data) {
  RefPtr<nsDragService> dragService = static_cast<nsDragService*>(data);
  return dragService->RemoveTempFiles();
}

NS_IMETHODIMP
nsDragService::EndDragSession(bool aDoneDrag, uint32_t aKeyModifiers) {
  LOGDRAGSERVICE("nsDragService::EndDragSession(%p) %d",
                 mTargetDragContext.get(), aDoneDrag);

  if (sGrabWidget) {
    g_signal_handlers_disconnect_by_func(
        sGrabWidget, FuncToGpointer(OnSourceGrabEventAfter), this);
    g_object_unref(sGrabWidget);
    sGrabWidget = nullptr;

    if (sMotionEventTimerID) {
      g_source_remove(sMotionEventTimerID);
      sMotionEventTimerID = 0;
    }
    if (sMotionEvent) {
      TakeMotionEvent();
    }
  }

  // unset our drag action
  SetDragAction(DRAGDROP_ACTION_NONE);

  // start timer to remove temporary files
  if (mTemporaryFiles.Count() > 0 && !mTempFileTimerID) {
    LOGDRAGSERVICE("  queue removing of temporary files");
    // |this| won't be used after nsDragService delete because the timer is
    // removed in the nsDragService destructor.
    mTempFileTimerID =
        g_timeout_add(NS_DND_TMP_CLEANUP_TIMEOUT, TaskRemoveTempFiles, this);
    mTempFileUrls.Clear();
  }

  // We're done with the drag context.
  if (mSourceWindow) {
    mSourceWindow->SetDragSource(nullptr);
    mSourceWindow = nullptr;
  }
  mTargetDragContextForRemote = nullptr;
  mTargetWindow = nullptr;
  mPendingWindow = nullptr;
  mCachedDragContext = 0;

  return nsBaseDragService::EndDragSession(aDoneDrag, aKeyModifiers);
}

// nsIDragSession
NS_IMETHODIMP
nsDragService::SetCanDrop(bool aCanDrop) {
  LOGDRAGSERVICE("nsDragService::SetCanDrop %d", aCanDrop);
  mCanDrop = aCanDrop;
  return NS_OK;
}

NS_IMETHODIMP
nsDragService::GetCanDrop(bool* aCanDrop) {
  LOGDRAGSERVICE("nsDragService::GetCanDrop");
  *aCanDrop = mCanDrop;
  return NS_OK;
}

static void UTF16ToNewUTF8(const char16_t* aUTF16, uint32_t aUTF16Len,
                           char** aUTF8, uint32_t* aUTF8Len) {
  nsDependentSubstring utf16(aUTF16, aUTF16Len);
  *aUTF8 = ToNewUTF8String(utf16, aUTF8Len);
}

static void UTF8ToNewUTF16(const char* aUTF8, uint32_t aUTF8Len,
                           char16_t** aUTF16, uint32_t* aUTF16Len) {
  nsDependentCSubstring utf8(aUTF8, aUTF8Len);
  *aUTF16 = UTF8ToNewUnicode(utf8, aUTF16Len);
}

// count the number of URIs in some text/uri-list format data.
static uint32_t CountTextUriListItems(const char* data, uint32_t datalen) {
  const char* p = data;
  const char* endPtr = p + datalen;
  uint32_t count = 0;

  while (p < endPtr) {
    // skip whitespace (if any)
    while (p < endPtr && *p != '\0' && isspace(*p)) p++;
    // if we aren't at the end of the line ...
    if (p != endPtr && *p != '\0' && *p != '\n' && *p != '\r') count++;
    // skip to the end of the line
    while (p < endPtr && *p != '\0' && *p != '\n') p++;
    p++;  // skip the actual newline as well.
  }
  return count;
}

// extract an item from text/uri-list formatted data and convert it to
// unicode.
static void GetTextUriListItem(const char* data, uint32_t datalen,
                               uint32_t aItemIndex, char16_t** convertedText,
                               uint32_t* convertedTextLen) {
  const char* p = data;
  const char* endPtr = p + datalen;
  unsigned int count = 0;

  *convertedText = nullptr;
  while (p < endPtr) {
    // skip whitespace (if any)
    while (p < endPtr && *p != '\0' && isspace(*p)) p++;
    // if we aren't at the end of the line, we have a url
    if (p != endPtr && *p != '\0' && *p != '\n' && *p != '\r') count++;
    // this is the item we are after ...
    if (aItemIndex + 1 == count) {
      const char* q = p;
      while (q < endPtr && *q != '\0' && *q != '\n' && *q != '\r') q++;
      UTF8ToNewUTF16(p, q - p, convertedText, convertedTextLen);
      break;
    }
    // skip to the end of the line
    while (p < endPtr && *p != '\0' && *p != '\n') p++;
    p++;  // skip the actual newline as well.
  }

  // didn't find the desired item, so just pass the whole lot
  if (!*convertedText) {
    UTF8ToNewUTF16(data, datalen, convertedText, convertedTextLen);
  }
}

// Spins event loop, called from JS.
// Can lead to another round of drag_motion events.
NS_IMETHODIMP
nsDragService::GetNumDropItems(uint32_t* aNumItems) {
  LOGDRAGSERVICE("nsDragService::GetNumDropItems");

  if (!mTargetWidget) {
    LOGDRAGSERVICE(
        "*** warning: GetNumDropItems \
               called without a valid target widget!\n");
    *aNumItems = 0;
    return NS_OK;
  }

  bool isList = IsTargetContextList();
  if (isList) {
    if (!mSourceDataItems) {
      *aNumItems = 0;
      return NS_OK;
    }
    mSourceDataItems->GetLength(aNumItems);
  } else {
    GdkAtom gdkFlavor = gdk_atom_intern(gTextUriListType, FALSE);
    if (!gdkFlavor) {
      *aNumItems = 0;
      return NS_OK;
    }

    nsTArray<nsCString> dragFlavors;
    GetDragFlavors(dragFlavors);
    GetTargetDragData(gdkFlavor, dragFlavors);

    if (mTargetDragData) {
      const char* data = reinterpret_cast<char*>(mTargetDragData);
      *aNumItems = CountTextUriListItems(data, mTargetDragDataLen);
    } else
      *aNumItems = 1;
  }
  LOGDRAGSERVICE("  NumOfDropItems %d", *aNumItems);
  return NS_OK;
}

void nsDragService::GetDragFlavors(nsTArray<nsCString>& aFlavors) {
  for (GList* tmp = gdk_drag_context_list_targets(mTargetDragContext); tmp;
       tmp = tmp->next) {
    GdkAtom atom = GDK_POINTER_TO_ATOM(tmp->data);
    GUniquePtr<gchar> name(gdk_atom_name(atom));
    if (!name) {
      continue;
    }
    aFlavors.AppendElement(nsCString(name.get()));
  }
}

// Spins event loop, called from JS.
// Can lead to another round of drag_motion events.
NS_IMETHODIMP
nsDragService::GetData(nsITransferable* aTransferable, uint32_t aItemIndex) {
  LOGDRAGSERVICE("nsDragService::GetData(), index %d", aItemIndex);

  // make sure that we have a transferable
  if (!aTransferable) {
    return NS_ERROR_INVALID_ARG;
  }

  if (!mTargetWidget) {
    LOGDRAGSERVICE(
        "*** failed: GetData called without a valid target widget!\n");
    return NS_ERROR_FAILURE;
  }

  // get flavor list that includes all acceptable flavors (including
  // ones obtained through conversion).
  nsTArray<nsCString> flavors;
  nsresult rv = aTransferable->FlavorsTransferableCanImport(flavors);
  if (NS_FAILED(rv)) {
    LOGDRAGSERVICE("  failed to get flavors, quit.");
    return rv;
  }

  // check to see if this is an internal list
  bool isList = IsTargetContextList();

  if (isList) {
    LOGDRAGSERVICE("  Process as a list...");
    // find a matching flavor
    for (uint32_t i = 0; i < flavors.Length(); ++i) {
      nsCString& flavorStr = flavors[i];
      LOGDRAGSERVICE("  [%d] flavor is %s\n", i, flavorStr.get());
      // get the item with the right index
      nsCOMPtr<nsITransferable> item =
          do_QueryElementAt(mSourceDataItems, aItemIndex);
      if (!item) continue;

      nsCOMPtr<nsISupports> data;
      LOGDRAGSERVICE("  trying to get transfer data for %s\n", flavorStr.get());
      rv = item->GetTransferData(flavorStr.get(), getter_AddRefs(data));
      if (NS_FAILED(rv)) {
        LOGDRAGSERVICE("  failed.\n");
        continue;
      }
      rv = aTransferable->SetTransferData(flavorStr.get(), data);
      if (NS_FAILED(rv)) {
        LOGDRAGSERVICE("  fail to set transfer data into transferable!\n");
        continue;
      }
      LOGDRAGSERVICE("  succeeded\n");
      // ok, we got the data
      return NS_OK;
    }
    // if we got this far, we failed
    LOGDRAGSERVICE("  failed to match flavors\n");
    return NS_ERROR_FAILURE;
  }

  nsTArray<nsCString> dragFlavors;
  GetDragFlavors(dragFlavors);

  // Now walk down the list of flavors. When we find one that is
  // actually present, copy out the data into the transferable in that
  // format. SetTransferData() implicitly handles conversions.
  for (uint32_t i = 0; i < flavors.Length(); ++i) {
    nsCString& flavorStr = flavors[i];

    GdkAtom gdkFlavor;
    if (flavorStr.EqualsLiteral(kTextMime)) {
      gdkFlavor = gdk_atom_intern(gTextPlainUTF8Type, FALSE);
    } else {
      gdkFlavor = gdk_atom_intern(flavorStr.get(), FALSE);
    }
    LOGDRAGSERVICE("  we're getting data %s (gdk flavor %p)\n", flavorStr.get(),
                   gdkFlavor);
    bool dataFound = false;
    if (gdkFlavor) {
      GetTargetDragData(gdkFlavor, dragFlavors);
    }
    if (mTargetDragData) {
      LOGDRAGSERVICE("  dataFound = true\n");
      dataFound = true;
    } else {
      LOGDRAGSERVICE("  dataFound = false, try conversions\n");

      // Dragging and dropping from the file manager would cause us
      // to parse the source text as a nsIFile URL.
      if (flavorStr.EqualsLiteral(kFileMime)) {
        LOGDRAGSERVICE("  conversion %s => %s", kFileMime, gTextUriListType);
        gdkFlavor = gdk_atom_intern(gTextUriListType, FALSE);
        GetTargetDragData(gdkFlavor, dragFlavors);
        if (mTargetDragData) {
          const char* text = static_cast<char*>(mTargetDragData);
          char16_t* convertedText = nullptr;
          uint32_t convertedTextLen = 0;

          GetTextUriListItem(text, mTargetDragDataLen, aItemIndex,
                             &convertedText, &convertedTextLen);

          if (convertedText) {
            nsCOMPtr<nsIIOService> ioService = do_GetIOService(&rv);
            nsCOMPtr<nsIURI> fileURI;
            rv = ioService->NewURI(NS_ConvertUTF16toUTF8(convertedText),
                                   nullptr, nullptr, getter_AddRefs(fileURI));
            if (NS_SUCCEEDED(rv)) {
              nsCOMPtr<nsIFileURL> fileURL = do_QueryInterface(fileURI, &rv);
              if (NS_SUCCEEDED(rv)) {
                nsCOMPtr<nsIFile> file;
                rv = fileURL->GetFile(getter_AddRefs(file));
                if (NS_SUCCEEDED(rv)) {
                  // The common wrapping code at the end of
                  // this function assumes the data is text
                  // and calls text-specific operations.
                  // Make a secret hideout here for nsIFile
                  // objects and return early.
                  LOGDRAGSERVICE("  set as file %s",
                                 NS_ConvertUTF16toUTF8(convertedText).get());
                  aTransferable->SetTransferData(flavorStr.get(), file);
                  g_free(convertedText);
                  return NS_OK;
                }
              }
            }
            g_free(convertedText);
          }
          continue;
        }
      }

      // If we are looking for text/plain, try again with non utf-8 text.
      if (flavorStr.EqualsLiteral(kTextMime)) {
        LOGDRAGSERVICE("  conversion %s => %s", kTextMime, kTextMime);
        gdkFlavor = gdk_atom_intern(kTextMime, FALSE);
        GetTargetDragData(gdkFlavor, dragFlavors);
        if (mTargetDragData) {
          dataFound = true;
        }  // if plain text flavor present
      }    // if looking for text/plain

      // if we are looking for text/x-moz-url and we failed to find
      // it on the clipboard, try again with text/uri-list, and then
      // _NETSCAPE_URL
      if (flavorStr.EqualsLiteral(kURLMime)) {
        LOGDRAGSERVICE("  conversion %s => %s", kURLMime, gTextUriListType);
        gdkFlavor = gdk_atom_intern(gTextUriListType, FALSE);
        GetTargetDragData(gdkFlavor, dragFlavors);
        if (mTargetDragData) {
          const char* data = reinterpret_cast<char*>(mTargetDragData);
          char16_t* convertedText = nullptr;
          uint32_t convertedTextLen = 0;

          GetTextUriListItem(data, mTargetDragDataLen, aItemIndex,
                             &convertedText, &convertedTextLen);

          if (convertedText) {
            // out with the old, in with the new
            g_free(mTargetDragData);
            mTargetDragData = convertedText;
            mTargetDragDataLen = convertedTextLen * 2;
            dataFound = true;
          }
        }
        if (!dataFound) {
          LOGDRAGSERVICE("  conversion %s => %s", kURLMime, gMozUrlType);
          gdkFlavor = gdk_atom_intern(gMozUrlType, FALSE);
          GetTargetDragData(gdkFlavor, dragFlavors);
          if (mTargetDragData) {
            const char* castedText = reinterpret_cast<char*>(mTargetDragData);
            char16_t* convertedText = nullptr;
            uint32_t convertedTextLen = 0;
            UTF8ToNewUTF16(castedText, mTargetDragDataLen, &convertedText,
                           &convertedTextLen);
            if (convertedText) {
              // out with the old, in with the new
              g_free(mTargetDragData);
              mTargetDragData = convertedText;
              mTargetDragDataLen = convertedTextLen * 2;
              dataFound = true;
            }
          }
        }
      }

    }  // else we try one last ditch effort to find our data

    if (dataFound) {
      LOGDRAGSERVICE("  actual data found %s\n",
                     GUniquePtr<gchar>(gdk_atom_name(gdkFlavor)).get());

      if (flavorStr.EqualsLiteral(kTextMime)) {
        // The text is in UTF-8, so convert the text into UTF-16
        const char* text = static_cast<char*>(mTargetDragData);
        NS_ConvertUTF8toUTF16 ucs2string(text, mTargetDragDataLen);
        char16_t* convertedText = ToNewUnicode(ucs2string, mozilla::fallible);
        if (convertedText) {
          g_free(mTargetDragData);
          mTargetDragData = convertedText;
          mTargetDragDataLen = ucs2string.Length() * 2;
        }
      }

      if (flavorStr.EqualsLiteral(kJPEGImageMime) ||
          flavorStr.EqualsLiteral(kJPGImageMime) ||
          flavorStr.EqualsLiteral(kPNGImageMime) ||
          flavorStr.EqualsLiteral(kGIFImageMime)) {
        LOGDRAGSERVICE("  saving as image %s\n", flavorStr.get());

        nsCOMPtr<nsIInputStream> byteStream;
        NS_NewByteInputStream(getter_AddRefs(byteStream),
                              Span((char*)mTargetDragData, mTargetDragDataLen),
                              NS_ASSIGNMENT_COPY);
        aTransferable->SetTransferData(flavorStr.get(), byteStream);
        continue;
      }

      if (!flavorStr.EqualsLiteral(kCustomTypesMime)) {
        // the DOM only wants LF, so convert from MacOS line endings
        // to DOM line endings.
        nsLinebreakHelpers::ConvertPlatformToDOMLinebreaks(
            flavorStr.EqualsLiteral(kRTFMime), &mTargetDragData,
            reinterpret_cast<int*>(&mTargetDragDataLen));
      }

      // put it into the transferable.
      nsCOMPtr<nsISupports> genericDataWrapper;
      nsPrimitiveHelpers::CreatePrimitiveForData(
          flavorStr, mTargetDragData, mTargetDragDataLen,
          getter_AddRefs(genericDataWrapper));
      aTransferable->SetTransferData(flavorStr.get(), genericDataWrapper);
      // we found one, get out of this loop!
      break;
    }
  }

  return NS_OK;
}

NS_IMETHODIMP
nsDragService::IsDataFlavorSupported(const char* aDataFlavor, bool* _retval) {
  LOGDRAGSERVICE("nsDragService::IsDataFlavorSupported(%p) %s",
                 mTargetDragContext.get(), aDataFlavor);
  if (!_retval) {
    return NS_ERROR_INVALID_ARG;
  }

  // set this to no by default
  *_retval = false;

  // check to make sure that we have a drag object set, here
  if (!mTargetWidget) {
    LOGDRAGSERVICE(
        "*** warning: IsDataFlavorSupported called without a valid target "
        "widget!\n");
    return NS_OK;
  }

  // check to see if the target context is a list.
  bool isList = IsTargetContextList();
  // if it is, just look in the internal data since we are the source
  // for it.
  if (isList) {
    LOGDRAGSERVICE("  It's a list");
    uint32_t numDragItems = 0;
    // if we don't have mDataItems we didn't start this drag so it's
    // an external client trying to fool us.
    if (!mSourceDataItems) {
      LOGDRAGSERVICE("  quit");
      return NS_OK;
    }
    mSourceDataItems->GetLength(&numDragItems);
    LOGDRAGSERVICE("  drag items %d", numDragItems);
    for (uint32_t itemIndex = 0; itemIndex < numDragItems; ++itemIndex) {
      nsCOMPtr<nsITransferable> currItem =
          do_QueryElementAt(mSourceDataItems, itemIndex);
      if (currItem) {
        nsTArray<nsCString> flavors;
        currItem->FlavorsTransferableCanExport(flavors);
        for (uint32_t i = 0; i < flavors.Length(); ++i) {
          LOGDRAGSERVICE("  checking %s against %s\n", flavors[i].get(),
                         aDataFlavor);
          if (flavors[i].Equals(aDataFlavor)) {
            LOGDRAGSERVICE("  found.\n");
            *_retval = true;
          }
        }
      }
    }
    return NS_OK;
  }

  // check the target context vs. this flavor, one at a time
  GList* tmp = nullptr;
  if (mTargetDragContext) {
    tmp = gdk_drag_context_list_targets(mTargetDragContext);
  }

  for (; tmp; tmp = tmp->next) {
    /* Bug 331198 */
    GdkAtom atom = GDK_POINTER_TO_ATOM(tmp->data);
    GUniquePtr<gchar> name(gdk_atom_name(atom));
    if (!name) {
      continue;
    }

    if (strcmp(name.get(), aDataFlavor) == 0) {
      *_retval = true;
    }
    // check for automatic text/uri-list -> text/x-moz-url mapping
    else if (strcmp(name.get(), gTextUriListType) == 0 &&
             (strcmp(aDataFlavor, kURLMime) == 0 ||
              strcmp(aDataFlavor, kFileMime) == 0)) {
      *_retval = true;
    }
    // check for automatic _NETSCAPE_URL -> text/x-moz-url mapping
    else if (strcmp(name.get(), gMozUrlType) == 0 &&
             (strcmp(aDataFlavor, kURLMime) == 0)) {
      *_retval = true;
    }

    if (*_retval) {
      LOGDRAGSERVICE("  supported, with converting %s => %s", name.get(),
                     aDataFlavor);
    }
  }

  if (!*_retval) {
    LOGDRAGSERVICE("  %s is not supported", aDataFlavor);
  }

  return NS_OK;
}

void nsDragService::ReplyToDragMotion(GdkDragContext* aDragContext,
                                      guint aTime) {
  LOGDRAGSERVICE("nsDragService::ReplyToDragMotion(%p) can drop %d",
                 aDragContext, mCanDrop);

  GdkDragAction action = (GdkDragAction)0;
  if (mCanDrop) {
    // notify the dragger if we can drop
    switch (mDragAction) {
      case DRAGDROP_ACTION_COPY:
        LOGDRAGSERVICE("  set explicit action copy");
        action = GDK_ACTION_COPY;
        break;
      case DRAGDROP_ACTION_LINK:
        LOGDRAGSERVICE("  set explicit action link");
        action = GDK_ACTION_LINK;
        break;
      case DRAGDROP_ACTION_NONE:
        LOGDRAGSERVICE("  set explicit action none");
        action = (GdkDragAction)0;
        break;
      default:
        LOGDRAGSERVICE("  set explicit action move");
        action = GDK_ACTION_MOVE;
        break;
    }
  } else {
    LOGDRAGSERVICE("  mCanDrop is false, disable drop");
  }

  // gdk_drag_status() is a kind of red herring here.
  // It does not control final D&D operation type (copy/move) but controls
  // drop/no-drop D&D state and default cursor type (copy/move).

  // Actual D&D operation is determined by mDragAction which is set by
  // SetDragAction() from UpdateDragAction() or gecko/layout.

  // State passed to gdk_drag_status() sets default D&D cursor type
  // which can be switched by key control (CTRL/SHIFT).
  // If user changes D&D cursor (and D&D operation) we're notified by
  // gdk_drag_context_get_selected_action() and update mDragAction.

  // But if we pass mDragAction back to gdk_drag_status() the D&D operation
  // becames locked and won't be returned when D&D modifiers (CTRL/SHIFT)
  // are released.

  // This gdk_drag_context_get_selected_action() -> gdk_drag_status() ->
  // gdk_drag_context_get_selected_action() cycle happens on Wayland.
  if (widget::GdkIsWaylandDisplay() && action == GDK_ACTION_COPY) {
    LOGDRAGSERVICE("  Wayland: switch copy to move");
    action = GDK_ACTION_MOVE;
  }

  LOGDRAGSERVICE("  gdk_drag_status() action %d", action);
  gdk_drag_status(aDragContext, action, aTime);
}

void nsDragService::EnsureCachedDataValidForContext(
    GdkDragContext* aDragContext) {
  if (mCachedDragContext != (uintptr_t)aDragContext) {
    mCachedData.Clear();
    mCachedDragContext = (uintptr_t)aDragContext;
  }
}

void nsDragService::TargetDataReceived(GtkWidget* aWidget,
                                       GdkDragContext* aContext, gint aX,
                                       gint aY,
                                       GtkSelectionData* aSelectionData,
                                       guint aInfo, guint32 aTime) {
  LOGDRAGSERVICE("nsDragService::TargetDataReceived(%p)", aContext);
  TargetResetData();

  EnsureCachedDataValidForContext(aContext);

  mTargetDragDataReceived = true;
  gint len = gtk_selection_data_get_length(aSelectionData);
  const guchar* data = gtk_selection_data_get_data(aSelectionData);

  GdkAtom target = gtk_selection_data_get_target(aSelectionData);
  GUniquePtr<gchar> name(gdk_atom_name(target));
  nsDependentCString flavor(name.get());

  LOGDRAGSERVICE("  got data, MIME %s", flavor.get());

  if (len > 0 && data) {
    mTargetDragDataLen = len;
    mTargetDragData = g_malloc(mTargetDragDataLen);
    memcpy(mTargetDragData, data, mTargetDragDataLen);

    LOGDRAGSERVICE("  got data, len = %d", mTargetDragDataLen);

    nsTArray<uint8_t> copy;
    if (!copy.SetLength(len, fallible)) {
      return;
    }
    memcpy(copy.Elements(), data, len);

    mCachedData.InsertOrUpdate(flavor, std::move(copy));
  } else {
    LOGDRAGSERVICE("Failed to get data. selection data len was %d\n",
                   mTargetDragDataLen);
    mCachedData.InsertOrUpdate(flavor, nsTArray<uint8_t>());
  }
}

bool nsDragService::IsTargetContextList(void) {
  bool retval = false;

  // gMimeListType drags only work for drags within a single process. The
  // gtk_drag_get_source_widget() function will return nullptr if the source
  // of the drag is another app, so we use it to check if a gMimeListType
  // drop will work or not.
  if (mTargetDragContext &&
      gtk_drag_get_source_widget(mTargetDragContext) == nullptr) {
    return retval;
  }

  GList* tmp = nullptr;
  if (mTargetDragContext) {
    tmp = gdk_drag_context_list_targets(mTargetDragContext);
  }

  // walk the list of context targets and see if one of them is a list
  // of items.
  for (; tmp; tmp = tmp->next) {
    /* Bug 331198 */
    GdkAtom atom = GDK_POINTER_TO_ATOM(tmp->data);
    GUniquePtr<gchar> name(gdk_atom_name(atom));
    if (name && !strcmp(name.get(), gMimeListType)) {
      return true;
    }
  }

  return retval;
}

// Spins event loop, called from eDragTaskMotion handler by
// DispatchMotionEvents().
// Can lead to another round of drag_motion events.
void nsDragService::GetTargetDragData(GdkAtom aFlavor,
                                      nsTArray<nsCString>& aDropFlavors) {
  LOGDRAGSERVICE("nsDragService::GetTargetDragData(%p) '%s'\n",
                 mTargetDragContext.get(),
                 GUniquePtr<gchar>(gdk_atom_name(aFlavor)).get());

  // reset our target data areas
  TargetResetData();

  GUniquePtr<gchar> name(gdk_atom_name(aFlavor));
  nsDependentCString flavor(name.get());

  // Return early when requested MIME is not offered by D&D.
  if (!aDropFlavors.Contains(flavor)) {
    LOGDRAGSERVICE("  %s is missing", flavor.get());
    return;
  }

  if (mTargetDragContext) {
    // We keep a copy of the requested data with the same life-time
    // as mTargetDragContext.
    // Especially with multiple items the same data is requested
    // very often.
    EnsureCachedDataValidForContext(mTargetDragContext);
    if (auto cached = mCachedData.Lookup(flavor)) {
      mTargetDragDataLen = cached->Length();
      LOGDRAGSERVICE("  using cached data for %s, length is %d", flavor.get(),
                     mTargetDragDataLen);

      if (mTargetDragDataLen) {
        mTargetDragData = g_malloc(mTargetDragDataLen);
        memcpy(mTargetDragData, cached->Elements(), mTargetDragDataLen);
      }

      mTargetDragDataReceived = true;
      LOGDRAGSERVICE("  %s found in cache", flavor.get());
      return;
    }

    gtk_drag_get_data(mTargetWidget, mTargetDragContext, aFlavor, mTargetTime);

    LOGDRAGSERVICE("  about to start inner iteration.");
    gtk_main_iteration();

    PRTime entryTime = PR_Now();
    while (!mTargetDragDataReceived && mDoingDrag) {
      // check the number of iterations
      LOGDRAGSERVICE("  doing iteration...\n");
      PR_Sleep(PR_MillisecondsToInterval(10)); /* sleep for 10 ms/iteration */
      if (PR_Now() - entryTime > NS_DND_TIMEOUT) {
        LOGDRAGSERVICE("  failed to get D&D data in time!\n");
        break;
      }
      gtk_main_iteration();
    }
  }

#ifdef MOZ_LOGGING
  if (mTargetDragDataLen && mTargetDragData) {
    LOGDRAGSERVICE("  %s got from system", flavor.get());
  } else {
    LOGDRAGSERVICE("  %s failed to get from system", flavor.get());
  }
#endif
}

void nsDragService::TargetResetData(void) {
  mTargetDragDataReceived = false;
  // make sure to free old data if we have to
  g_free(mTargetDragData);
  mTargetDragData = 0;
  mTargetDragDataLen = 0;
}

static void TargetArrayAddTarget(nsTArray<GtkTargetEntry*>& aTargetArray,
                                 const char* aTarget) {
  GtkTargetEntry* target = (GtkTargetEntry*)g_malloc(sizeof(GtkTargetEntry));
  target->target = g_strdup(aTarget);
  target->flags = 0;
  aTargetArray.AppendElement(target);
  LOGDRAGSERVICESTATIC("adding target %s\n", aTarget);
}

static bool CanExportAsURLTarget(const char16_t* aURLData, uint32_t aURLLen) {
  for (const nsLiteralString& disallowed : kDisallowedExportedSchemes) {
    auto len = disallowed.AsString().Length();
    if (len < aURLLen) {
      if (!memcmp(disallowed.get(), aURLData,
                  /* len is char16_t char count */ len * 2)) {
        LOGDRAGSERVICESTATIC("rejected URL scheme %s\n",
                             NS_ConvertUTF16toUTF8(disallowed).get());
        return false;
      }
    }
  }
  return true;
}

GtkTargetList* nsDragService::GetSourceList(void) {
  if (!mSourceDataItems) {
    return nullptr;
  }

  nsTArray<GtkTargetEntry*> targetArray;
  GtkTargetEntry* targets;
  GtkTargetList* targetList = 0;
  uint32_t targetCount = 0;
  unsigned int numDragItems = 0;

  mSourceDataItems->GetLength(&numDragItems);
  LOGDRAGSERVICE("  numDragItems = %d", numDragItems);

  // Check to see if we're dragging > 1 item.
  if (numDragItems > 1) {
    // as the Xdnd protocol only supports a single item (or is it just
    // gtk's implementation?), we don't advertise all flavours listed
    // in the nsITransferable.

    // the application/x-moz-internal-item-list format, which preserves
    // all information for drags within the same mozilla instance.
    TargetArrayAddTarget(targetArray, gMimeListType);

    // check what flavours are supported so we can decide what other
    // targets to advertise.
    nsCOMPtr<nsITransferable> currItem = do_QueryElementAt(mSourceDataItems, 0);

    if (currItem) {
      nsTArray<nsCString> flavors;
      currItem->FlavorsTransferableCanExport(flavors);
      for (uint32_t i = 0; i < flavors.Length(); ++i) {
        // check if text/x-moz-url is supported.
        // If so, advertise
        // text/uri-list.
        if (flavors[i].EqualsLiteral(kURLMime)) {
          TargetArrayAddTarget(targetArray, gTextUriListType);
          break;
        }
      }
    }  // if item is a transferable
  } else if (numDragItems == 1) {
    nsCOMPtr<nsITransferable> currItem = do_QueryElementAt(mSourceDataItems, 0);
    if (currItem) {
      nsTArray<nsCString> flavors;
      currItem->FlavorsTransferableCanExport(flavors);
      for (uint32_t i = 0; i < flavors.Length(); ++i) {
        nsCString& flavorStr = flavors[i];

        TargetArrayAddTarget(targetArray, flavorStr.get());

        // If there is a file, add the text/uri-list type.
        if (flavorStr.EqualsLiteral(kFileMime)) {
          TargetArrayAddTarget(targetArray, gTextUriListType);
        }
        // Check to see if this is text/plain.
        else if (flavorStr.EqualsLiteral(kTextMime)) {
          TargetArrayAddTarget(targetArray, gTextPlainUTF8Type);
        }
        // Check to see if this is the x-moz-url type.
        // If it is, add _NETSCAPE_URL
        // this is a type used by everybody.
        else if (flavorStr.EqualsLiteral(kURLMime)) {
          nsCOMPtr<nsISupports> data;
          if (NS_SUCCEEDED(currItem->GetTransferData(flavorStr.get(),
                                                     getter_AddRefs(data)))) {
            void* tmpData = nullptr;
            uint32_t tmpDataLen = 0;
            nsPrimitiveHelpers::CreateDataFromPrimitive(
                nsDependentCString(flavorStr.get()), data, &tmpData,
                &tmpDataLen);
            if (tmpData) {
              if (CanExportAsURLTarget(reinterpret_cast<char16_t*>(tmpData),
                                       tmpDataLen / 2)) {
                TargetArrayAddTarget(targetArray, gMozUrlType);
              }
              free(tmpData);
            }
          }
        }
        // check if application/x-moz-file-promise url is supported.
        // If so, advertise text/uri-list.
        else if (flavorStr.EqualsLiteral(kFilePromiseURLMime)) {
          TargetArrayAddTarget(targetArray, gTextUriListType);
        }
        // XdndDirectSave, use on X.org only.
        else if (widget::GdkIsX11Display() && !widget::IsXWaylandProtocol() &&
                 flavorStr.EqualsLiteral(kFilePromiseMime)) {
          TargetArrayAddTarget(targetArray, gXdndDirectSaveType);
        }
        // kNativeImageMime
        else if (flavorStr.EqualsLiteral(kNativeImageMime)) {
          TargetArrayAddTarget(targetArray, kPNGImageMime);
          TargetArrayAddTarget(targetArray, kJPEGImageMime);
          TargetArrayAddTarget(targetArray, kJPGImageMime);
          TargetArrayAddTarget(targetArray, kGIFImageMime);
        }
      }
    }
  }

  // get all the elements that we created.
  targetCount = targetArray.Length();
  if (targetCount) {
    // allocate space to create the list of valid targets
    targets = (GtkTargetEntry*)g_malloc(sizeof(GtkTargetEntry) * targetCount);
    uint32_t targetIndex;
    for (targetIndex = 0; targetIndex < targetCount; ++targetIndex) {
      GtkTargetEntry* disEntry = targetArray.ElementAt(targetIndex);
      // this is a string reference but it will be freed later.
      targets[targetIndex].target = disEntry->target;
      targets[targetIndex].flags = disEntry->flags;
      targets[targetIndex].info = 0;
    }
    targetList = gtk_target_list_new(targets, targetCount);
    // clean up the target list
    for (uint32_t cleanIndex = 0; cleanIndex < targetCount; ++cleanIndex) {
      GtkTargetEntry* thisTarget = targetArray.ElementAt(cleanIndex);
      g_free(thisTarget->target);
      g_free(thisTarget);
    }
    g_free(targets);
  } else {
    // We need to create a dummy target list to be able initialize dnd.
    targetList = gtk_target_list_new(nullptr, 0);
  }
  return targetList;
}

void nsDragService::SourceEndDragSession(GdkDragContext* aContext,
                                         gint aResult) {
  LOGDRAGSERVICE("SourceEndDragSession(%p) result %s\n", aContext,
                 kGtkDragResults[aResult]);

  // this just releases the list of data items that we provide
  mSourceDataItems = nullptr;

  // Remove this property, if it exists, to satisfy the Direct Save Protocol.
  GdkAtom property = gdk_atom_intern(gXdndDirectSaveType, FALSE);
  gdk_property_delete(gdk_drag_context_get_source_window(aContext), property);

  if (!mDoingDrag || mScheduledTask == eDragTaskSourceEnd)
    // EndDragSession() was already called on drop
    // or SourceEndDragSession on drag-failed
    return;

  if (mEndDragPoint.x < 0) {
    // We don't have a drag end point, so guess
    gint x, y;
    GdkDisplay* display = gdk_display_get_default();
    GdkScreen* screen = gdk_display_get_default_screen(display);
    GtkWindow* window = GetGtkWindow(mSourceDocument);
    GdkWindow* gdkWindow = window ? gtk_widget_get_window(GTK_WIDGET(window))
                                  : gdk_screen_get_root_window(screen);
    gdk_window_get_device_position(
        gdkWindow, gdk_drag_context_get_device(aContext), &x, &y, nullptr);
    gint scale = gdk_window_get_scale_factor(gdkWindow);
    SetDragEndPoint(LayoutDeviceIntPoint(x * scale, y * scale));
    LOGDRAGSERVICE("  guess drag end point %d %d\n", x * scale, y * scale);
  }

  // Either the drag was aborted or the drop occurred outside the app.
  // The dropEffect of mDataTransfer is not updated for motion outside the
  // app, but is needed for the dragend event, so set it now.

  uint32_t dropEffect;

  if (aResult == GTK_DRAG_RESULT_SUCCESS) {
    LOGDRAGSERVICE("  drop is accepted");
    // With GTK+ versions 2.10.x and prior the drag may have been
    // cancelled (but no drag-failed signal would have been sent).
    // aContext->dest_window will be non-nullptr only if the drop was
    // sent.
    GdkDragAction action = gdk_drag_context_get_dest_window(aContext)
                               ? gdk_drag_context_get_actions(aContext)
                               : (GdkDragAction)0;

    // Only one bit of action should be set, but, just in case someone
    // does something funny, erring away from MOVE, and not recording
    // unusual action combinations as NONE.
    if (!action) {
      LOGDRAGSERVICE("  drop action is none");
      dropEffect = DRAGDROP_ACTION_NONE;
    } else if (action & GDK_ACTION_COPY) {
      LOGDRAGSERVICE("  drop action is copy");
      dropEffect = DRAGDROP_ACTION_COPY;
    } else if (action & GDK_ACTION_LINK) {
      LOGDRAGSERVICE("  drop action is link");
      dropEffect = DRAGDROP_ACTION_LINK;
    } else if (action & GDK_ACTION_MOVE) {
      LOGDRAGSERVICE("  drop action is move");
      dropEffect = DRAGDROP_ACTION_MOVE;
    } else {
      LOGDRAGSERVICE("  drop action is copy");
      dropEffect = DRAGDROP_ACTION_COPY;
    }
  } else {
    LOGDRAGSERVICE("  drop action is none");
    dropEffect = DRAGDROP_ACTION_NONE;
    if (aResult != GTK_DRAG_RESULT_NO_TARGET) {
      LOGDRAGSERVICE("  drop is user chancelled\n");
      mUserCancelled = true;
    }
  }

  if (mDataTransfer) {
    mDataTransfer->SetDropEffectInt(dropEffect);
  }

  // Schedule the appropriate drag end dom events.
  Schedule(eDragTaskSourceEnd, nullptr, nullptr, LayoutDeviceIntPoint(), 0);
}

static nsresult GetDownloadDetails(nsITransferable* aTransferable,
                                   nsIURI** aSourceURI, nsAString& aFilename) {
  *aSourceURI = nullptr;
  MOZ_ASSERT(aTransferable != nullptr, "aTransferable must not be null");

  // get the URI from the kFilePromiseURLMime flavor
  nsCOMPtr<nsISupports> urlPrimitive;
  nsresult rv = aTransferable->GetTransferData(kFilePromiseURLMime,
                                               getter_AddRefs(urlPrimitive));
  if (NS_FAILED(rv)) {
    return NS_ERROR_FAILURE;
  }
  nsCOMPtr<nsISupportsString> srcUrlPrimitive = do_QueryInterface(urlPrimitive);
  if (!srcUrlPrimitive) {
    return NS_ERROR_FAILURE;
  }

  nsAutoString srcUri;
  srcUrlPrimitive->GetData(srcUri);
  if (srcUri.IsEmpty()) {
    return NS_ERROR_FAILURE;
  }
  nsCOMPtr<nsIURI> sourceURI;
  NS_NewURI(getter_AddRefs(sourceURI), srcUri);

  nsAutoString srcFileName;
  nsCOMPtr<nsISupports> fileNamePrimitive;
  rv = aTransferable->GetTransferData(kFilePromiseDestFilename,
                                      getter_AddRefs(fileNamePrimitive));
  if (NS_FAILED(rv)) {
    return NS_ERROR_FAILURE;
  }
  nsCOMPtr<nsISupportsString> srcFileNamePrimitive =
      do_QueryInterface(fileNamePrimitive);
  if (srcFileNamePrimitive) {
    srcFileNamePrimitive->GetData(srcFileName);
  } else {
    nsCOMPtr<nsIURL> sourceURL = do_QueryInterface(sourceURI);
    if (!sourceURL) {
      return NS_ERROR_FAILURE;
    }
    nsAutoCString urlFileName;
    sourceURL->GetFileName(urlFileName);
    NS_UnescapeURL(urlFileName);
    CopyUTF8toUTF16(urlFileName, srcFileName);
  }
  if (srcFileName.IsEmpty()) {
    return NS_ERROR_FAILURE;
  }

  sourceURI.swap(*aSourceURI);
  aFilename = srcFileName;
  return NS_OK;
}

// See nsContentAreaDragDropDataProvider::GetFlavorData() for reference.
nsresult nsDragService::CreateTempFile(nsITransferable* aItem,
                                       nsACString& aURI) {
  LOGDRAGSERVICE("nsDragService::CreateTempFile()");

  nsCOMPtr<nsIFile> tmpDir;
  nsresult rv = NS_GetSpecialDirectory(NS_OS_TEMP_DIR, getter_AddRefs(tmpDir));
  if (NS_FAILED(rv)) {
    LOGDRAGSERVICE("  Failed to get temp directory\n");
    return rv;
  }

  nsCOMPtr<nsIInputStream> inputStream;
  nsCOMPtr<nsIChannel> channel;

  // extract the file name and source uri of the promise-file data
  nsAutoString wideFileName;
  nsCOMPtr<nsIURI> sourceURI;
  rv = GetDownloadDetails(aItem, getter_AddRefs(sourceURI), wideFileName);
  if (NS_FAILED(rv)) {
    LOGDRAGSERVICE(
        "  Failed to extract file name and source uri from download url");
    return rv;
  }

  // Check if the file is already stored at /tmp.
  // It happens when drop destination is changed and SourceDataGet() is caled
  // more than once.
  nsAutoCString fileName;
  CopyUTF16toUTF8(wideFileName, fileName);
  auto fileLen = fileName.Length();
  for (const auto& url : mTempFileUrls) {
    auto URLLen = url.Length();
    if (URLLen > fileLen &&
        fileName.Equals(nsDependentCString(url, URLLen - fileLen))) {
      aURI = url;
      LOGDRAGSERVICE("  recycle file %s", PromiseFlatCString(aURI).get());
      return NS_OK;
    }
  }

  // create and open channel for source uri
  nsCOMPtr<nsIPrincipal> principal = aItem->GetRequestingPrincipal();
  nsContentPolicyType contentPolicyType = aItem->GetContentPolicyType();
  nsCOMPtr<nsICookieJarSettings> cookieJarSettings =
      aItem->GetCookieJarSettings();
  rv = NS_NewChannel(getter_AddRefs(channel), sourceURI, principal,
                     nsILoadInfo::SEC_ALLOW_CROSS_ORIGIN_SEC_CONTEXT_IS_NULL,
                     contentPolicyType, cookieJarSettings);
  if (NS_FAILED(rv)) {
    LOGDRAGSERVICE("  Failed to create new channel for source uri");
    return rv;
  }

  rv = channel->Open(getter_AddRefs(inputStream));
  if (NS_FAILED(rv)) {
    LOGDRAGSERVICE("  Failed to open channel for source uri");
    return rv;
  }

  // build the file:///tmp/dnd_file URL
  tmpDir->Append(NS_LITERAL_STRING_FROM_CSTRING("dnd_file"));
  rv = tmpDir->CreateUnique(nsIFile::DIRECTORY_TYPE, 0700);
  if (NS_FAILED(rv)) {
    LOGDRAGSERVICE("  Failed create tmp dir");
    return rv;
  }

  // store a copy of that temporary directory so we can
  // clean them up when nsDragService is destructed
  nsCOMPtr<nsIFile> tempFile;
  tmpDir->Clone(getter_AddRefs(tempFile));
  mTemporaryFiles.AppendObject(tempFile);
  if (mTempFileTimerID) {
    g_source_remove(mTempFileTimerID);
    mTempFileTimerID = 0;
  }

  // extend file:///tmp/dnd_file/<filename> URL
  tmpDir->Append(wideFileName);

  nsCOMPtr<nsIOutputStream> outputStream;
  rv = NS_NewLocalFileOutputStream(getter_AddRefs(outputStream), tmpDir);
  if (NS_FAILED(rv)) {
    LOGDRAGSERVICE("  Failed to open output stream for temporary file");
    return rv;
  }

  char buffer[8192];
  uint32_t readCount = 0;
  uint32_t writeCount = 0;
  while (1) {
    rv = inputStream->Read(buffer, sizeof(buffer), &readCount);
    if (NS_FAILED(rv)) {
      LOGDRAGSERVICE("  Failed to read data from source uri");
      return rv;
    }

    if (readCount == 0) break;

    rv = outputStream->Write(buffer, readCount, &writeCount);
    if (NS_FAILED(rv)) {
      LOGDRAGSERVICE("  Failed to write data to temporary file");
      return rv;
    }
  }

  inputStream->Close();
  rv = outputStream->Close();
  if (NS_FAILED(rv)) {
    LOGDRAGSERVICE("  Failed to write data to temporary file");
    return rv;
  }

  nsCOMPtr<nsIURI> uri;
  rv = NS_NewFileURI(getter_AddRefs(uri), tmpDir);
  if (NS_FAILED(rv)) {
    LOGDRAGSERVICE("  Failed to get file URI");
    return rv;
  }
  nsCOMPtr<nsIURL> fileURL(do_QueryInterface(uri));
  if (!fileURL) {
    LOGDRAGSERVICE("  Failed to query file interface");
    return NS_ERROR_FAILURE;
  }
  rv = fileURL->GetSpec(aURI);
  if (NS_FAILED(rv)) {
    LOGDRAGSERVICE("  Failed to get filepath");
    return rv;
  }

  // store url of temporary file
  mTempFileUrls.AppendElement()->Assign(aURI);
  LOGDRAGSERVICE("  storing tmp file as %s", PromiseFlatCString(aURI).get());
  return NS_OK;
}

bool nsDragService::SourceDataAppendURLFileItem(nsACString& aURI,
                                                nsITransferable* aItem) {
  // If there is a file available, create a URI from the file.
  nsCOMPtr<nsISupports> data;
  nsresult rv = aItem->GetTransferData(kFileMime, getter_AddRefs(data));
  NS_ENSURE_SUCCESS(rv, false);
  if (nsCOMPtr<nsIFile> file = do_QueryInterface(data)) {
    nsCOMPtr<nsIURI> fileURI;
    NS_NewFileURI(getter_AddRefs(fileURI), file);
    if (fileURI) {
      fileURI->GetSpec(aURI);
      return true;
    }
  }
  return false;
}

bool nsDragService::SourceDataAppendURLItem(nsITransferable* aItem,
                                            bool aExternalDrop,
                                            nsACString& aURI) {
  nsCOMPtr<nsISupports> data;
  nsresult rv = aItem->GetTransferData(kURLMime, getter_AddRefs(data));
  if (NS_FAILED(rv)) {
    return SourceDataAppendURLFileItem(aURI, aItem);
  }

  nsCOMPtr<nsISupportsString> string = do_QueryInterface(data);
  if (!string) {
    return false;
  }

  nsAutoString text;
  string->GetData(text);
  if (!aExternalDrop || CanExportAsURLTarget(text.get(), text.Length())) {
    AppendUTF16toUTF8(text, aURI);
    return true;
  }

  // We're dropping to another application and the URL can't be exported
  // as it's internal one (mailbox:// etc.)
  // Try to get file target directly.
  if (SourceDataAppendURLFileItem(aURI, aItem)) {
    return true;
  }

  // We can't get the file directly so try to download it and save to tmp.
  // The desktop or file manager expects for drags of promise-file data
  // the text/uri-list flavor set to a temporary file that contains the
  // promise-file data.
  // We open a stream on the <protocol>:// url here and save the content
  // to file:///tmp/dnd_file/<filename> and pass this url
  // as text/uri-list flavor.

  // check whether transferable contains FilePromiseUrl flavor...
  nsCOMPtr<nsISupports> promiseData;
  rv = aItem->GetTransferData(kFilePromiseURLMime, getter_AddRefs(promiseData));
  NS_ENSURE_SUCCESS(rv, false);

  // ... if so, create a temporary file and pass its url
  return NS_SUCCEEDED(CreateTempFile(aItem, aURI));
}

void nsDragService::SourceDataGetUriList(GdkDragContext* aContext,
                                         GtkSelectionData* aSelectionData,
                                         uint32_t aDragItems) {
  // Check if we're transfering data to another application.
  // gdk_drag_context_get_dest_window() on X11 returns GdkWindow even for
  // different application so use nsWindow::GetWindow() to check if that's
  // our window.
  const bool isExternalDrop =
      widget::GdkIsX11Display()
          ? !nsWindow::GetWindow(gdk_drag_context_get_dest_window(aContext))
          : !gdk_drag_context_get_dest_window(aContext);

  LOGDRAGSERVICE("nsDragService::SourceDataGetUriLists() len %d external %d",
                 aDragItems, isExternalDrop);

  // Disable processing of native events until we store all files to /tmp.
  // Otherwise user can quit drop before we have all files saved
  // and that cancels whole D&D.
  AutoSuspendNativeEvents suspend;

  nsAutoCString uriList;
  for (uint32_t i = 0; i < aDragItems; i++) {
    nsCOMPtr<nsITransferable> item = do_QueryElementAt(mSourceDataItems, i);
    if (!item) {
      continue;
    }
    nsAutoCString uri;
    if (!SourceDataAppendURLItem(item, isExternalDrop, uri)) {
      continue;
    }
    // text/x-moz-url is of form url + "\n" + title.
    // We just want the url.
    int32_t separatorPos = uri.FindChar(u'\n');
    if (separatorPos >= 0) {
      uri.Truncate(separatorPos);
    }
    uriList.Append(uri);
    uriList.AppendLiteral("\r\n");
  }

  LOGDRAGSERVICE("URI list\n%s", uriList.get());
  GdkAtom target = gtk_selection_data_get_target(aSelectionData);
  gtk_selection_data_set(aSelectionData, target, 8, (guchar*)uriList.get(),
                         uriList.Length());
}

void nsDragService::SourceDataGetImage(nsITransferable* aItem,
                                       GtkSelectionData* aSelectionData) {
  LOGDRAGSERVICE("nsDragService::SourceDataGetImage()");

  nsresult rv;
  nsCOMPtr<nsISupports> data;
  rv = aItem->GetTransferData(kNativeImageMime, getter_AddRefs(data));
  NS_ENSURE_SUCCESS_VOID(rv);

  LOGDRAGSERVICE("  posting image\n");
  nsCOMPtr<imgIContainer> image = do_QueryInterface(data);
  if (!image) {
    LOGDRAGSERVICE("  do_QueryInterface failed\n");
    return;
  }
  RefPtr<GdkPixbuf> pixbuf = nsImageToPixbuf::ImageToPixbuf(image);
  if (!pixbuf) {
    LOGDRAGSERVICE("  ImageToPixbuf failed\n");
    return;
  }
  gtk_selection_data_set_pixbuf(aSelectionData, pixbuf);
  LOGDRAGSERVICE("  image data set\n");
  return;
}

void nsDragService::SourceDataGetXDND(nsITransferable* aItem,
                                      GdkDragContext* aContext,
                                      GtkSelectionData* aSelectionData) {
  LOGDRAGSERVICE("nsDragService::SourceDataGetXDND");

  // Indicate failure by default.
  GdkAtom target = gtk_selection_data_get_target(aSelectionData);
  gtk_selection_data_set(aSelectionData, target, 8, (guchar*)"E", 1);

  GdkAtom property = gdk_atom_intern(gXdndDirectSaveType, FALSE);
  GdkAtom type = gdk_atom_intern(kTextMime, FALSE);

  GdkWindow* srcWindow = gdk_drag_context_get_source_window(aContext);
  if (!srcWindow) {
    LOGDRAGSERVICE("  failed to get source GdkWindow!");
    return;
  }

  // Ensure null termination.
  nsAutoCString data;
  {
    GUniquePtr<guchar> gdata;
    gint length = 0;
    if (!gdk_property_get(srcWindow, property, type, 0, INT32_MAX, FALSE,
                          nullptr, nullptr, &length, getter_Transfers(gdata))) {
      LOGDRAGSERVICE("  failed to get gXdndDirectSaveType GdkWindow property.");
      return;
    }
    data.Assign(nsDependentCSubstring((const char*)gdata.get(), length));
  }

  GUniquePtr<char> hostname;
  GUniquePtr<char> fullpath(
      g_filename_from_uri(data.get(), getter_Transfers(hostname), nullptr));
  if (!fullpath) {
    LOGDRAGSERVICE("  failed to get file from uri.");
    return;
  }

  // If there is no hostname in the URI, NULL will be stored.
  // We should not accept uris with from a different host.
  if (hostname) {
    nsCOMPtr<nsIPropertyBag2> infoService =
        do_GetService(NS_SYSTEMINFO_CONTRACTID);
    if (!infoService) {
      return;
    }
    nsAutoCString host;
    if (NS_SUCCEEDED(infoService->GetPropertyAsACString(u"host"_ns, host))) {
      if (!host.Equals(hostname.get())) {
        LOGDRAGSERVICE("  ignored drag because of different host.");
        // Special error code "F" for this case.
        gtk_selection_data_set(aSelectionData, target, 8, (guchar*)"F", 1);
        return;
      }
    }
  }

  LOGDRAGSERVICE("  XdndDirectSave filepath is %s", fullpath.get());

  nsCOMPtr<nsIFile> file;
  if (NS_FAILED(NS_NewNativeLocalFile(nsDependentCString(fullpath.get()), false,
                                      getter_AddRefs(file)))) {
    LOGDRAGSERVICE("  failed to get local file");
    return;
  }

  // We have to split the path into a directory and filename,
  // because our internal file-promise API is based on these.
  nsCOMPtr<nsIFile> directory;
  file->GetParent(getter_AddRefs(directory));

  aItem->SetTransferData(kFilePromiseDirectoryMime, directory);

  nsCOMPtr<nsISupportsString> filenamePrimitive =
      do_CreateInstance(NS_SUPPORTS_STRING_CONTRACTID);
  if (!filenamePrimitive) {
    return;
  }

  nsAutoString leafName;
  file->GetLeafName(leafName);
  filenamePrimitive->SetData(leafName);

  aItem->SetTransferData(kFilePromiseDestFilename, filenamePrimitive);

  nsCOMPtr<nsISupports> promiseData;
  nsresult rv =
      aItem->GetTransferData(kFilePromiseMime, getter_AddRefs(promiseData));
  NS_ENSURE_SUCCESS_VOID(rv);

  // Indicate success.
  gtk_selection_data_set(aSelectionData, target, 8, (guchar*)"S", 1);
  return;
}

bool nsDragService::SourceDataGetText(nsITransferable* aItem,
                                      const nsACString& aMIMEType,
                                      bool aNeedToDoConversionToPlainText,
                                      GtkSelectionData* aSelectionData) {
  LOGDRAGSERVICE("nsDragService::SourceDataGetPlain()");

  nsresult rv;
  nsCOMPtr<nsISupports> data;
  rv = aItem->GetTransferData(PromiseFlatCString(aMIMEType).get(),
                              getter_AddRefs(data));
  NS_ENSURE_SUCCESS(rv, false);

  void* tmpData = nullptr;
  uint32_t tmpDataLen = 0;

  nsPrimitiveHelpers::CreateDataFromPrimitive(aMIMEType, data, &tmpData,
                                              &tmpDataLen);
  // if required, do the extra work to convert unicode to plain
  // text and replace the output values with the plain text.
  if (aNeedToDoConversionToPlainText) {
    char* plainTextData = nullptr;
    char16_t* castedUnicode = reinterpret_cast<char16_t*>(tmpData);
    uint32_t plainTextLen = 0;
    UTF16ToNewUTF8(castedUnicode, tmpDataLen / 2, &plainTextData,
                   &plainTextLen);
    if (tmpData) {
      // this was not allocated using glib
      free(tmpData);
      tmpData = plainTextData;
      tmpDataLen = plainTextLen;
    }
  }
  if (tmpData) {
    // this copies the data
    GdkAtom target = gtk_selection_data_get_target(aSelectionData);
    gtk_selection_data_set(aSelectionData, target, 8, (guchar*)tmpData,
                           tmpDataLen);
    // this wasn't allocated with glib
    free(tmpData);
  }

  return true;
}

// We're asked to get data from mSourceDataItems and pass it to
// GtkSelectionData (Gtk D&D interface).
// We need to check mSourceDataItems data type and try to convert it
// to data type accepted by Gtk.
void nsDragService::SourceDataGet(GtkWidget* aWidget, GdkDragContext* aContext,
                                  GtkSelectionData* aSelectionData,
                                  guint32 aTime) {
  LOGDRAGSERVICE("nsDragService::SourceDataGet(%p)", aContext);

  GdkAtom target = gtk_selection_data_get_target(aSelectionData);
  GUniquePtr<gchar> requestedTypeName(gdk_atom_name(target));
  if (!requestedTypeName) {
    LOGDRAGSERVICE("  failed to get atom name.\n");
    return;
  }

  LOGDRAGSERVICE("  Gtk asks us for %s data type\n", requestedTypeName.get());
  // check to make sure that we have data items to return.
  if (!mSourceDataItems) {
    LOGDRAGSERVICE("  Failed to get our data items\n");
    return;
  }

  uint32_t dragItems;
  mSourceDataItems->GetLength(&dragItems);
  LOGDRAGSERVICE("  source data items %d", dragItems);

  nsDependentCString mimeFlavor(requestedTypeName.get());
  if (mimeFlavor.EqualsLiteral(gTextUriListType)) {
    SourceDataGetUriList(aContext, aSelectionData, dragItems);
    return;
  }

#ifdef MOZ_LOGGING
  if (dragItems > 1) {
    LOGDRAGSERVICE(
        "  There are %d data items but we're asked for %s MIME type. Only "
        "first data element can be transfered!",
        dragItems, mimeFlavor.get());
  }
#endif

  nsCOMPtr<nsITransferable> item = do_QueryElementAt(mSourceDataItems, 0);
  if (!item) {
    LOGDRAGSERVICE("  Failed to get SourceDataItems!");
    return;
  }

  if (mimeFlavor.EqualsLiteral(kTextMime) ||
      mimeFlavor.EqualsLiteral(gTextPlainUTF8Type)) {
    SourceDataGetText(item, nsDependentCString(kTextMime),
                      /* aNeedToDoConversionToPlainText */ true,
                      aSelectionData);
    // no fallback for text mime types
    return;
  }
  // Someone is asking for the special Direct Save Protocol type.
  else if (mimeFlavor.EqualsLiteral(gXdndDirectSaveType)) {
    SourceDataGetXDND(item, aContext, aSelectionData);
    // no fallback for XDND mime types
    return;
  } else if (mimeFlavor.EqualsLiteral(kPNGImageMime) ||
             mimeFlavor.EqualsLiteral(kJPEGImageMime) ||
             mimeFlavor.EqualsLiteral(kJPGImageMime) ||
             mimeFlavor.EqualsLiteral(kGIFImageMime)) {
    // no fallback for image mime types
    SourceDataGetImage(item, aSelectionData);
    return;
  } else if (mimeFlavor.EqualsLiteral(gMozUrlType)) {
    // Someone was asking for _NETSCAPE_URL. We need to get it from
    // transferable as x-moz-url and convert it to plain text.
    // No need to check
    if (SourceDataGetText(item, nsDependentCString(kURLMime),
                          /* aNeedToDoConversionToPlainText */ true,
                          aSelectionData)) {
      return;
    }
  }
  // Just try to get and set whatever we're asked for.
  SourceDataGetText(item, mimeFlavor,
                    /* aNeedToDoConversionToPlainText */ false, aSelectionData);
}

void nsDragService::SourceBeginDrag(GdkDragContext* aContext) {
  LOGDRAGSERVICE("nsDragService::SourceBeginDrag(%p)\n", aContext);

  nsCOMPtr<nsITransferable> transferable =
      do_QueryElementAt(mSourceDataItems, 0);
  if (!transferable) return;

  nsTArray<nsCString> flavors;
  nsresult rv = transferable->FlavorsTransferableCanImport(flavors);
  NS_ENSURE_SUCCESS(rv, );

  for (uint32_t i = 0; i < flavors.Length(); ++i) {
    if (flavors[i].EqualsLiteral(kFilePromiseDestFilename)) {
      nsCOMPtr<nsISupports> data;
      rv = transferable->GetTransferData(kFilePromiseDestFilename,
                                         getter_AddRefs(data));
      if (NS_FAILED(rv)) {
        LOGDRAGSERVICE("  transferable doesn't contain '%s",
                       kFilePromiseDestFilename);
        return;
      }

      nsCOMPtr<nsISupportsString> fileName = do_QueryInterface(data);
      if (!fileName) {
        LOGDRAGSERVICE("  failed to get file name");
        return;
      }

      nsAutoString fileNameStr;
      fileName->GetData(fileNameStr);

      nsCString fileNameCStr;
      CopyUTF16toUTF8(fileNameStr, fileNameCStr);

      GdkAtom property = gdk_atom_intern(gXdndDirectSaveType, FALSE);
      GdkAtom type = gdk_atom_intern(kTextMime, FALSE);

      gdk_property_change(gdk_drag_context_get_source_window(aContext),
                          property, type, 8, GDK_PROP_MODE_REPLACE,
                          (const guchar*)fileNameCStr.get(),
                          fileNameCStr.Length());
      break;
    }
  }
}

void nsDragService::SetDragIcon(GdkDragContext* aContext) {
  if (!mHasImage && !mSelection) return;

  LOGDRAGSERVICE("nsDragService::SetDragIcon(%p)", aContext);

  LayoutDeviceIntRect dragRect;
  nsPresContext* pc;
  RefPtr<SourceSurface> surface;
  DrawDrag(mSourceNode, mRegion, mScreenPosition, &dragRect, &surface, &pc);
  if (!pc) {
    LOGDRAGSERVICE("  PresContext is missing!");
    return;
  }

  const auto screenPoint =
      LayoutDeviceIntPoint::Round(mScreenPosition * pc->CSSToDevPixelScale());
  int32_t offsetX = screenPoint.x - dragRect.x;
  int32_t offsetY = screenPoint.y - dragRect.y;

  // If a popup is set as the drag image, use its widget. Otherwise, use
  // the surface that DrawDrag created.
  //
  // XXX: Disable drag popups on GTK 3.19.4 and above: see bug 1264454.
  //      Fix this once a new GTK version ships that does not destroy our
  //      widget in gtk_drag_set_icon_widget.
  //      This is fixed in GTK 3.24
  //      by
  //      https://gitlab.gnome.org/GNOME/gtk/-/commit/c27c4e2048acb630feb24c31288f802345e99f4c
  bool gtk_drag_set_icon_widget_is_working =
      gtk_check_version(3, 19, 4) != nullptr ||
      gtk_check_version(3, 24, 0) == nullptr;
  if (mDragPopup && gtk_drag_set_icon_widget_is_working) {
    GtkWidget* gtkWidget = nullptr;
    nsIFrame* frame = mDragPopup->GetPrimaryFrame();
    if (frame) {
      // DrawDrag ensured that this is a popup frame.
      nsCOMPtr<nsIWidget> widget = frame->GetNearestWidget();
      if (widget) {
        gtkWidget = (GtkWidget*)widget->GetNativeData(NS_NATIVE_SHELLWIDGET);
        if (gtkWidget) {
          // When mDragPopup has a parent it's already attached to D&D context.
          // That may happens when D&D operation is aborted but not finished
          // on Gtk side yet so let's remove it now.
          if (GtkWidget* parent = gtk_widget_get_parent(gtkWidget)) {
            gtk_container_remove(GTK_CONTAINER(parent), gtkWidget);
          }
          LOGDRAGSERVICE("  set drag popup [%p]", widget.get());
          OpenDragPopup();
          gtk_drag_set_icon_widget(aContext, gtkWidget, offsetX, offsetY);
          return;
        } else {
          LOGDRAGSERVICE("  NS_NATIVE_SHELLWIDGET is missing!");
        }
      } else {
        LOGDRAGSERVICE("  NearestWidget is missing!");
      }
    } else {
      LOGDRAGSERVICE("  PrimaryFrame is missing!");
    }
  }

  if (surface) {
    LOGDRAGSERVICE("  We have a surface");
    if (!SetAlphaPixmap(surface, aContext, offsetX, offsetY, dragRect)) {
      RefPtr<GdkPixbuf> dragPixbuf = nsImageToPixbuf::SourceSurfaceToPixbuf(
          surface, dragRect.width, dragRect.height);
      if (dragPixbuf) {
        LOGDRAGSERVICE("  set drag pixbuf");
        gtk_drag_set_icon_pixbuf(aContext, dragPixbuf, offsetX, offsetY);
      } else {
        LOGDRAGSERVICE("  SourceSurfaceToPixbuf failed!");
      }
    }
  } else {
    LOGDRAGSERVICE("  Surface is missing!");
  }
}

static void invisibleSourceDragBegin(GtkWidget* aWidget,
                                     GdkDragContext* aContext, gpointer aData) {
  LOGDRAGSERVICESTATIC("invisibleSourceDragBegin (%p)", aContext);
  nsDragService* dragService = (nsDragService*)aData;

  dragService->SourceBeginDrag(aContext);
  dragService->SetDragIcon(aContext);
}

static void invisibleSourceDragDataGet(GtkWidget* aWidget,
                                       GdkDragContext* aContext,
                                       GtkSelectionData* aSelectionData,
                                       guint aInfo, guint32 aTime,
                                       gpointer aData) {
  LOGDRAGSERVICESTATIC("invisibleSourceDragDataGet (%p)", aContext);
  nsDragService* dragService = (nsDragService*)aData;
  dragService->SourceDataGet(aWidget, aContext, aSelectionData, aTime);
}

static gboolean invisibleSourceDragFailed(GtkWidget* aWidget,
                                          GdkDragContext* aContext,
                                          gint aResult, gpointer aData) {
#ifdef MOZ_WAYLAND
  // Wayland and X11 uses different drag results here. When drag target is
  // missing X11 passes GDK_DRAG_CANCEL_NO_TARGET
  // (from gdk_dnd_handle_button_event()/gdkdnd-x11.c)
  // as backend X11 has info about other application windows.
  // Wayland does not have such info so it always passes
  // GDK_DRAG_CANCEL_ERROR error code
  // (see data_source_cancelled/gdkselection-wayland.c).
  // Bug 1527976
  if (widget::GdkIsWaylandDisplay() && aResult == GTK_DRAG_RESULT_ERROR) {
    for (GList* tmp = gdk_drag_context_list_targets(aContext); tmp;
         tmp = tmp->next) {
      GdkAtom atom = GDK_POINTER_TO_ATOM(tmp->data);
      GUniquePtr<gchar> name(gdk_atom_name(atom));
      if (name && !strcmp(name.get(), gTabDropType)) {
        aResult = GTK_DRAG_RESULT_NO_TARGET;
        LOGDRAGSERVICESTATIC("invisibleSourceDragFailed(%p): Wayland tab drop",
                             aContext);
        break;
      }
    }
  }
#endif
  LOGDRAGSERVICESTATIC("invisibleSourceDragFailed(%p) %s", aContext,
                       kGtkDragResults[aResult]);
  nsDragService* dragService = (nsDragService*)aData;
  // End the drag session now (rather than waiting for the drag-end signal)
  // so that operations performed on dropEffect == none can start immediately
  // rather than waiting for the drag-failed animation to finish.
  dragService->SourceEndDragSession(aContext, aResult);

  // We should return TRUE to disable the drag-failed animation iff the
  // source performed an operation when dropEffect was none, but the handler
  // of the dragend DOM event doesn't provide this information.
  return FALSE;
}

static void invisibleSourceDragEnd(GtkWidget* aWidget, GdkDragContext* aContext,
                                   gpointer aData) {
  LOGDRAGSERVICESTATIC("invisibleSourceDragEnd(%p)", aContext);
  nsDragService* dragService = (nsDragService*)aData;

  // The drag has ended.  Release the hostages!
  dragService->SourceEndDragSession(aContext, GTK_DRAG_RESULT_SUCCESS);
}

// The following methods handle responding to GTK drag signals and
// tracking state between these signals.
//
// In general, GTK does not expect us to run the event loop while handling its
// drag signals, however our drag event handlers may run the
// event loop, most often to fetch information about the drag data.
//
// GTK, for example, uses the return value from drag-motion signals to
// determine whether drag-leave signals should be sent.  If an event loop is
// run during drag-motion the XdndLeave message can get processed but when GTK
// receives the message it does not yet know that it needs to send the
// drag-leave signal to our widget.
//
// After a drag-drop signal, we need to reply with gtk_drag_finish().
// However, gtk_drag_finish should happen after the drag-drop signal handler
// returns so that when the Motif drag protocol is used, the
// XmTRANSFER_SUCCESS during gtk_drag_finish is sent after the XmDROP_START
// reply sent on return from the drag-drop signal handler.
//
// Similarly drag-end for a successful drag and drag-failed are not good
// times to run a nested event loop as gtk_drag_drop_finished() and
// gtk_drag_source_info_destroy() don't gtk_drag_clear_source_info() or remove
// drop_timeout until after at least the first of these signals is sent.
// Processing other events (e.g. a slow GDK_DROP_FINISHED reply, or the drop
// timeout) could cause gtk_drag_drop_finished to be called again with the
// same GtkDragSourceInfo, which won't like being destroyed twice.
//
// Therefore we reply to the signals immediately and schedule a task to
// dispatch the Gecko events, which may run the event loop.
//
// Action in response to drag-leave signals is also delayed until the event
// loop runs again so that we find out whether a drag-drop signal follows.
//
// A single task is scheduled to manage responses to all three GTK signals.
// If further signals are received while the task is scheduled, the scheduled
// response is updated, sometimes effectively compressing successive signals.
//
// No Gecko drag events are dispatched (during nested event loops) while other
// Gecko drag events are in flight.  This helps event handlers that may not
// expect nested events, while accessing an event's dataTransfer for example.

gboolean nsDragService::ScheduleMotionEvent(nsWindow* aWindow,
                                            GdkDragContext* aDragContext,
                                            LayoutDeviceIntPoint aWindowPoint,
                                            guint aTime) {
  if (aDragContext && mScheduledTask == eDragTaskMotion) {
    // The drag source has sent another motion message before we've
    // replied to the previous.  That shouldn't happen with Xdnd.  The
    // spec for Motif drags is less clear, but we'll just update the
    // scheduled task with the new position reply only to the most
    // recent message.
    NS_WARNING("Drag Motion message received before previous reply was sent");
  }

  // Returning TRUE means we'll reply with a status message, unless we first
  // get a leave.
  return Schedule(eDragTaskMotion, aWindow, aDragContext, aWindowPoint, aTime);
}

void nsDragService::ScheduleLeaveEvent() {
  // We don't know at this stage whether a drop signal will immediately
  // follow.  If the drop signal gets sent it will happen before we return
  // to the main loop and the scheduled leave task will be replaced.
  if (!Schedule(eDragTaskLeave, nullptr, nullptr, LayoutDeviceIntPoint(), 0)) {
    NS_WARNING("Drag leave after drop");
  }
}

gboolean nsDragService::ScheduleDropEvent(nsWindow* aWindow,
                                          GdkDragContext* aDragContext,
                                          LayoutDeviceIntPoint aWindowPoint,
                                          guint aTime) {
  if (!Schedule(eDragTaskDrop, aWindow, aDragContext, aWindowPoint, aTime)) {
    NS_WARNING("Additional drag drop ignored");
    return FALSE;
  }

  SetDragEndPoint(aWindowPoint);

  // We'll reply with gtk_drag_finish().
  return TRUE;
}

#ifdef MOZ_LOGGING
const char* nsDragService::GetDragServiceTaskName(DragTask aTask) {
  static const char* taskNames[] = {"eDragTaskNone", "eDragTaskMotion",
                                    "eDragTaskLeave", "eDragTaskDrop",
                                    "eDragTaskSourceEnd"};
  MOZ_ASSERT(size_t(aTask) < ArrayLength(taskNames));
  return taskNames[aTask];
}
#endif

gboolean nsDragService::Schedule(DragTask aTask, nsWindow* aWindow,
                                 GdkDragContext* aDragContext,
                                 LayoutDeviceIntPoint aWindowPoint,
                                 guint aTime) {
  // If there is an existing leave or motion task scheduled, then that
  // will be replaced.  When the new task is run, it will dispatch
  // any necessary leave or motion events.

  // If aTask is eDragTaskSourceEnd, then it will replace even a scheduled
  // drop event (which could happen if the drop event has not been processed
  // within the allowed time).  Otherwise, if we haven't yet run a scheduled
  // drop or end task, just say that we are not ready to receive another
  // drop.
  LOGDRAGSERVICE("nsDragService::Schedule(%p) task %s window %p\n",
                 aDragContext, GetDragServiceTaskName(aTask), aWindow);

  if (mScheduledTask == eDragTaskSourceEnd ||
      (mScheduledTask == eDragTaskDrop && aTask != eDragTaskSourceEnd)) {
    LOGDRAGSERVICE("   task does not fit recent task %s, quit!\n",
                   GetDragServiceTaskName(mScheduledTask));
    return FALSE;
  }

  mScheduledTask = aTask;
  mPendingWindow = aWindow;
  mPendingDragContext = aDragContext;
  mPendingWindowPoint = aWindowPoint;
  mPendingTime = aTime;

  if (!mTaskSource) {
    // High priority is used here because we want to process motion events
    // right after drag_motion event handler which is called by Gtk.
    // An ideal scenario is to call TaskDispatchCallback() directly here
    // but we can't do that. TaskDispatchCallback() spins gtk event loop
    // while nsDragService::Schedule() is already called from event loop
    // (by drag_motion* gtk_widget events) so that direct call will cause
    // nested recursion.
    mTaskSource = g_timeout_add_full(G_PRIORITY_HIGH, 0, TaskDispatchCallback,
                                     this, nullptr);
  }

  // We need to reply to drag_motion event on Wayland immediately,
  // see Bug 1730203.
  if (widget::GdkIsWaylandDisplay() && mScheduledTask == eDragTaskMotion) {
    UpdateDragAction(aDragContext);
    ReplyToDragMotion(aDragContext, aTime);
  }

  return TRUE;
}

gboolean nsDragService::TaskDispatchCallback(gpointer data) {
  RefPtr<nsDragService> dragService = static_cast<nsDragService*>(data);
  AutoEventLoop loop(dragService);
  return dragService->RunScheduledTask();
}

gboolean nsDragService::RunScheduledTask() {
  LOGDRAGSERVICE(
      "nsDragService::RunScheduledTask() task %s mTargetWindow %p "
      "mPendingWindow %p\n",
      GetDragServiceTaskName(mScheduledTask), mTargetWindow.get(),
      mPendingWindow.get());

  // Don't run RunScheduledTask() twice. As we use it in main thread only
  // we don't need to be thread safe here.
  if (mScheduledTaskIsRunning) {
    LOGDRAGSERVICE("  sheduled task is already running, quit.");
    return FALSE;
  }
  AutoRestore<bool> guard(mScheduledTaskIsRunning);
  mScheduledTaskIsRunning = true;

  if (mTargetWindow && mTargetWindow != mPendingWindow) {
    LOGDRAGSERVICE("  dispatch eDragExit (%p)\n", mTargetWindow.get());
    mTargetWindow->DispatchDragEvent(eDragExit, mTargetWindowPoint, 0);

    if (!mSourceNode) {
      // The drag that was initiated in a different app. End the drag
      // session, since we're done with it for now (until the user drags
      // back into this app).
      EndDragSession(false, GetCurrentModifiers());
    }
  }

  // It is possible that the pending state has been updated during dispatch
  // of the leave event.  That's fine.

  // Now we collect the pending state because, from this point on, we want
  // to use the same state for all events dispatched.  All state is updated
  // so that when other tasks are scheduled during dispatch here, this
  // task is considered to have already been run.
  bool positionHasChanged = mPendingWindow != mTargetWindow ||
                            mPendingWindowPoint != mTargetWindowPoint;
  DragTask task = mScheduledTask;
  mScheduledTask = eDragTaskNone;
  mTargetWindow = std::move(mPendingWindow);
  mTargetWindowPoint = mPendingWindowPoint;

  if (task == eDragTaskLeave || task == eDragTaskSourceEnd) {
    LOGDRAGSERVICE("  quit, selected task %s\n", GetDragServiceTaskName(task));
    if (task == eDragTaskSourceEnd) {
      // Dispatch drag end events.
      EndDragSession(true, GetCurrentModifiers());
    }

    // Nothing more to do
    // Returning false removes the task source from the event loop.
    mTaskSource = 0;
    return FALSE;
  }

  // This may be the start of a destination drag session.
  StartDragSession();

  // mTargetWidget may be nullptr if the window has been destroyed.
  // (The leave event is not scheduled if a drop task is still scheduled.)
  // We still reply appropriately to indicate that the drop will or didn't
  // succeeed.
  mTargetWidget = mTargetWindow ? mTargetWindow->GetGtkWidget() : nullptr;
  LOGDRAGSERVICE("  start drag session mTargetWindow %p mTargetWidget %p\n",
                 mTargetWindow.get(), mTargetWidget.get());
  LOGDRAGSERVICE("  mPendingDragContext %p => mTargetDragContext %p\n",
                 mPendingDragContext.get(), mTargetDragContext.get());
  mTargetDragContext = std::move(mPendingDragContext);
  mTargetTime = mPendingTime;

  // http://www.whatwg.org/specs/web-apps/current-work/multipage/dnd.html#drag-and-drop-processing-model
  // (as at 27 December 2010) indicates that a "drop" event should only be
  // fired (at the current target element) if the current drag operation is
  // not none.  The current drag operation will only be set to a non-none
  // value during a "dragover" event.
  //
  // If the user has ended the drag before any dragover events have been
  // sent, then the spec recommends skipping the drop (because the current
  // drag operation is none).  However, here we assume that, by releasing
  // the mouse button, the user has indicated that they want to drop, so we
  // proceed with the drop where possible.
  //
  // In order to make the events appear to content in the same way as if the
  // spec is being followed we make sure to dispatch a "dragover" event with
  // appropriate coordinates and check canDrop before the "drop" event.
  //
  // When the Xdnd protocol is used for source/destination communication (as
  // should be the case with GTK source applications) a dragover event
  // should have already been sent during the drag-motion signal, which
  // would have already been received because XdndDrop messages do not
  // contain a position.  However, we can't assume the same when the Motif
  // protocol is used.
  if (task == eDragTaskMotion || positionHasChanged) {
    LOGDRAGSERVICE("  process motion event\n");
    UpdateDragAction();
    TakeDragEventDispatchedToChildProcess();  // Clear the old value.
    DispatchMotionEvents();
    if (task == eDragTaskMotion) {
      if (TakeDragEventDispatchedToChildProcess()) {
        mTargetDragContextForRemote = mTargetDragContext;
      } else {
        // Reply to tell the source whether we can drop and what
        // action would be taken.
        ReplyToDragMotion();
      }
    }
  }

  if (task == eDragTaskDrop) {
    LOGDRAGSERVICE("  process drop task\n");
    gboolean success = DispatchDropEvent();

    // Perhaps we should set the del parameter to TRUE when the drag
    // action is move, but we don't know whether the data was successfully
    // transferred.
    if (mTargetDragContext) {
      LOGDRAGSERVICE("  drag finished\n");
      gtk_drag_finish(mTargetDragContext, success,
                      /* del = */ FALSE, mTargetTime);
    }
    // Make sure to end the drag session. If this drag started in a
    // different app, we won't get a drag_end signal to end it from.
    EndDragSession(true, GetCurrentModifiers());
  }

  // We're done with the drag context.
  LOGDRAGSERVICE("  clear mTargetWindow mTargetWidget and other data\n");
  mTargetWidget = nullptr;
  mTargetDragContext = nullptr;

  // If we got another drag signal while running the sheduled task, that
  // must have happened while running a nested event loop.  Leave the task
  // source on the event loop.
  if (mScheduledTask != eDragTaskNone) return TRUE;

  // We have no task scheduled.
  // Returning false removes the task source from the event loop.
  LOGDRAGSERVICE("  remove task source\n");
  mTaskSource = 0;
  return FALSE;
}

// This will update the drag action based on the information in the
// drag context.  Gtk gets this from a combination of the key settings
// and what the source is offering.

void nsDragService::UpdateDragAction(GdkDragContext* aDragContext) {
  // This doesn't look right.  dragSession.dragAction is used by
  // nsContentUtils::SetDataTransferInEvent() to set the initial
  // dataTransfer.dropEffect, so GdkDragContext::suggested_action would be
  // more appropriate.  GdkDragContext::actions should be used to set
  // dataTransfer.effectAllowed, which doesn't currently happen with
  // external sources.
  LOGDRAGSERVICE("nsDragService::UpdateDragAction(%p)", aDragContext);

  // default is to do nothing
  int action = nsIDragService::DRAGDROP_ACTION_NONE;
  GdkDragAction gdkAction = GDK_ACTION_DEFAULT;
  if (aDragContext) {
    gdkAction = gdk_drag_context_get_actions(aDragContext);
    LOGDRAGSERVICE("  gdk_drag_context_get_actions() returns 0x%X", gdkAction);

    // When D&D modifiers (CTRL/SHIFT) are involved,
    // gdk_drag_context_get_actions() on X11 returns selected action but
    // Wayland returns all allowed actions.

    // So we need to call gdk_drag_context_get_selected_action() on Wayland
    // to get potential D&D modifier.
    // gdk_drag_context_get_selected_action() is also affected by
    // gdk_drag_status(), see nsDragService::ReplyToDragMotion().
    if (widget::GdkIsWaylandDisplay()) {
      GdkDragAction gdkActionSelected =
          gdk_drag_context_get_selected_action(aDragContext);
      LOGDRAGSERVICE("  gdk_drag_context_get_selected_action() returns 0x%X",
                     gdkActionSelected);
      if (gdkActionSelected) {
        gdkAction = gdkActionSelected;
      }
    }
  }

  // set the default just in case nothing matches below
  if (gdkAction & GDK_ACTION_DEFAULT) {
    LOGDRAGSERVICE("  set default move");
    action = nsIDragService::DRAGDROP_ACTION_MOVE;
  }
  // first check to see if move is set
  if (gdkAction & GDK_ACTION_MOVE) {
    LOGDRAGSERVICE("  set explicit move");
    action = nsIDragService::DRAGDROP_ACTION_MOVE;
  } else if (gdkAction & GDK_ACTION_LINK) {
    // then fall to the others
    LOGDRAGSERVICE("  set explicit link");
    action = nsIDragService::DRAGDROP_ACTION_LINK;
  } else if (gdkAction & GDK_ACTION_COPY) {
    // copy is ctrl
    LOGDRAGSERVICE("  set explicit copy");
    action = nsIDragService::DRAGDROP_ACTION_COPY;
  }

  // update the drag information
  SetDragAction(action);
}

void nsDragService::UpdateDragAction() { UpdateDragAction(mTargetDragContext); }

NS_IMETHODIMP
nsDragService::UpdateDragEffect() {
  LOGDRAGSERVICE("nsDragService::UpdateDragEffect() from e10s child process");
  if (mTargetDragContextForRemote) {
    ReplyToDragMotion(mTargetDragContextForRemote, mTargetTime);
    mTargetDragContextForRemote = nullptr;
  }
  return NS_OK;
}

void nsDragService::ReplyToDragMotion() {
  if (mTargetDragContext) {
    ReplyToDragMotion(mTargetDragContext, mTargetTime);
  }
}

void nsDragService::DispatchMotionEvents() {
  FireDragEventAtSource(eDrag, GetCurrentModifiers());
  if (mTargetWindow) {
    mTargetWindow->DispatchDragEvent(eDragOver, mTargetWindowPoint,
                                     mTargetTime);
  }
}

// Returns true if the drop was successful
gboolean nsDragService::DispatchDropEvent() {
  // We need to check IsDestroyed here because the nsRefPtr
  // only protects this from being deleted, it does NOT protect
  // against nsView::~nsView() calling Destroy() on it, bug 378273.
  if (!mTargetWindow || mTargetWindow->IsDestroyed()) {
    return FALSE;
  }

  EventMessage msg = mCanDrop ? eDrop : eDragExit;

  mTargetWindow->DispatchDragEvent(msg, mTargetWindowPoint, mTargetTime);

  return mCanDrop;
}

/* static */
uint32_t nsDragService::GetCurrentModifiers() {
  return mozilla::widget::KeymapWrapper::ComputeCurrentKeyModifiers();
}

#undef LOGDRAGSERVICE
