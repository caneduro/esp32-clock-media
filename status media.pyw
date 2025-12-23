import asyncio
import aiohttp
import time
import unicodedata
import threading
import sys
import queue
from ctypes import cast, POINTER
from comtypes import CLSCTX_ALL
from typing import Optional

# winsdk for media sessions (Windows-only)
try:
    from winsdk.windows.media.control import (
        GlobalSystemMediaTransportControlsSessionManager as MediaManager,
        GlobalSystemMediaTransportControlsSessionPlaybackStatus as PlaybackStatus
    )
    WINSKD_AVAILABLE = True
except Exception:
    WINSKD_AVAILABLE = False

# pycaw / comtypes for volume
try:
    from pycaw.pycaw import AudioUtilities, IAudioEndpointVolume
    PYCW_AVAILABLE = True
except Exception:
    PYCW_AVAILABLE = False

# System tray functionality
try:
    import pystray
    from PIL import Image, ImageDraw
    TRAY_AVAILABLE = True
except ImportError:
    TRAY_AVAILABLE = False

# ---------------- CONFIG ----------------
ESP32_IP_DEFAULT = "192.168.1.160"
SEND_TIMEOUT_S = 3
VOLUME_DELTA_TO_SEND = 1  
POSITION_SEND_THRESHOLD = 0.3  
POLL_FALLBACK_INTERVAL_S = 1.0
VOLUME_POLL_INTERVAL_S = 1.0  
INCLUDE_ASCII_DEFAULT = False
DEBUG_DEFAULT = True

# previous state cache
prev_state = {
    "title": None,
    "artist": None,
    "status": None,
    "position_sent": None,
    "duration": None,
    "volume": None
}

# ascii fallback map (small)
CYRILLIC_MAP = {
    'А':'A','Б':'B','В':'V','Г':'G','Д':'D','Е':'E','Ё':'E','Ж':'Zh','З':'Z','И':'I','Й':'I',
    'К':'K','Л':'L','М':'M','Н':'N','О':'O','П':'P','Р':'R','С':'S','Т':'T','У':'U','Ф':'F',
    'Х':'Kh','Ц':'Ts','Ч':'Ch','Ш':'Sh','Щ':'Shch','Ъ':'','Ы':'Y','Ь':'','Э':'E','Ю':'Yu','Я':'Ya',
    'а':'a','б':'b','в':'v','г':'g','д':'d','е':'e','ё':'e','ж':'zh','з':'z','и':'i','й':'i',
    'к':'k','л':'l','м':'m','н':'n','о':'o','п':'p','р':'r','с':'s','т':'t','у':'u','ф':'f',
    'х':'kh','ц':'ts','ч':'ch','ш':'sh','щ':'shch','ъ':'','ы':'y','ь':'','э':'e','ю':'yu','я':'ya'
}

def transliterate_basic(s: str) -> str:
    if not s:
        return s
    normalized = unicodedata.normalize('NFKD', s)
    out_chars = []
    for ch in normalized:
        if ord(ch) < 128:
            out_chars.append(ch)
        elif ch in CYRILLIC_MAP:
            out_chars.append(CYRILLIC_MAP[ch])
    return ' '.join(''.join(out_chars).split())

# GUI-async communication queue
gui_q = queue.Queue()

# Global flags
GUI_DEBUG = DEBUG_DEFAULT
INCLUDE_ASCII = INCLUDE_ASCII_DEFAULT
ESP32_IP = ESP32_IP_DEFAULT

def get_esp32_url():
    return f"http://{ESP32_IP}/update"

# aiohttp session + semaphore
_aio_sess: Optional[aiohttp.ClientSession] = None
_aio_sem = asyncio.Semaphore(4)

async def aio_init():
    global _aio_sess
    if _aio_sess is None:
        _aio_sess = aiohttp.ClientSession(connector=aiohttp.TCPConnector(limit=8))

async def send_payload(payload: dict) -> bool:
    await aio_init()
    url = get_esp32_url()
    try:
        async with _aio_sem:
            async with _aio_sess.post(url, json=payload, timeout=SEND_TIMEOUT_S) as resp:
                ok = resp.status == 200
                if GUI_DEBUG:
                    print(f"[DEBUG] Sent to {url}: {payload} -> Status: {resp.status}")
                return ok
    except Exception as e:
        if GUI_DEBUG:
            print(f"[DEBUG] Send failed: {e}")
        return False

# synchronous volume query (CORRETTA)
def query_system_volume_sync() -> int:
    if not PYCW_AVAILABLE:
        print("[VOLUME] pycaw non disponibile")
        return 0
    try:
        device = AudioUtilities.GetSpeakers()
        volume_interface = device.EndpointVolume
        vol_scalar = volume_interface.GetMasterVolumeLevelScalar()
        vol_percent = int(vol_scalar * 100)
        if GUI_DEBUG:
            print(f"[VOLUME] Lettura riuscita: {vol_percent}% (scalar={vol_scalar})")
        return vol_percent
    except Exception as e:
        print(f"[VOLUME] Errore lettura volume: {e}")
        return 0

# ---------------- Core decision & send logic ----------------
async def gather_and_send_from_session(session, send_fields: dict, reason="event"):
    global prev_state
    if not session:
        if GUI_DEBUG:
            print(f"[DEBUG] Nessuna sessione attiva per {reason}")
        return

    try:
        props = await session.try_get_media_properties_async()
        title = props.title or "Sconosciuto"
        artist = props.artist or "Sconosciuto"
        if ";" in artist:
            artist = ", ".join([a.strip() for a in artist.split(";")])

        playback_info = session.get_playback_info()
        status_map = {
            PlaybackStatus.PLAYING: "PLAY",
            PlaybackStatus.PAUSED: "PAUSE",
            PlaybackStatus.STOPPED: "STOP"
        }
        status = status_map.get(playback_info.playback_status, "STOP") if playback_info else "STOP"

        position = 0.0
        duration = 0.0
        try:
            timeline = session.get_timeline_properties()
            if timeline:
                if timeline.position:
                    position = float(timeline.position.total_seconds())
                if timeline.end_time:
                    duration = float(timeline.end_time.total_seconds())
        except Exception as e:
            if GUI_DEBUG:
                print(f"[DEBUG] Timeline error: {e}")

        vol = query_system_volume_sync()

        if GUI_DEBUG:
            print(f"[GATHER] {reason} → title='{title}', artist='{artist}', status={status}, pos={position:.1f}s, dur={duration:.1f}s, vol={vol}%")

        # Decidi se inviare
        should_send = False
        position_to_send = False

        # Cambiamenti metadata / status / duration / volume
        if (send_fields.get("title") and title != prev_state["title"]) or \
           (send_fields.get("artist") and artist != prev_state["artist"]):
            should_send = True
        if send_fields.get("status") and status != prev_state["status"]:
            should_send = True
        if send_fields.get("duration") and abs(duration - (prev_state["duration"] or 0)) > 0.5:
            should_send = True
        if send_fields.get("volume") and abs(vol - (prev_state["volume"] or -1)) >= VOLUME_DELTA_TO_SEND:
            should_send = True

        # Posizione: invia SOLO se utile
        pos_sent = prev_state.get("position_sent")
        if send_fields.get("position"):
            if pos_sent is None or abs(position - pos_sent) >= POSITION_SEND_THRESHOLD:
                position_to_send = True
            elif status != prev_state.get("status") and status in ("PLAY", "PAUSE"):
                position_to_send = True

        # Ignora position < 0.1s (inizio brano non significativo)
        if position_to_send and position < 0.1:
            if GUI_DEBUG:
                print("[DEBUG] Ignorato position < 0.1")
            position_to_send = False

        if should_send or position_to_send:
            payload = {}
            if send_fields.get("title"): payload["title"] = title
            if send_fields.get("artist"): payload["artist"] = artist
            if send_fields.get("status"): payload["status"] = status
            if send_fields.get("duration"): payload["duration"] = round(duration, 3)
            if position_to_send and send_fields.get("position"):
                payload["position"] = round(position, 3)
            if send_fields.get("volume"):
                payload["volume"] = vol

            if send_fields.get("ascii"):
                payload["title_ascii"] = transliterate_basic(title)
                payload["artist_ascii"] = transliterate_basic(artist)

            if GUI_DEBUG:
                print(f"[SEND] Invio: {payload}")
            ok = await send_payload(payload)
            if ok:
                if send_fields.get("title"): prev_state["title"] = title
                if send_fields.get("artist"): prev_state["artist"] = artist
                if send_fields.get("status"): prev_state["status"] = status
                if send_fields.get("duration"): prev_state["duration"] = duration
                if send_fields.get("volume"): prev_state["volume"] = vol
                if position_to_send: prev_state["position_sent"] = position
        else:
            if GUI_DEBUG:
                print("[DEBUG] Nessun cambiamento significativo → skip")

    except Exception as e:
        print(f"[ERROR] gather_and_send fallito: {e}")

# ---------------- volume callback registration (pycaw) ----------------
if PYCW_AVAILABLE:
    from comtypes import COMMETHOD, GUID
    from comtypes import COMObject

    IID_IAudioEndpointVolumeCallback = GUID("{657804FA-D6AD-4496-8A60-352752AF4F89}")

    class AudioEndpointVolumeCallback(COMObject):
        _com_interfaces_ = [IID_IAudioEndpointVolumeCallback]

        def __init__(self, loop, on_notify_corofunc):
            super().__init__()
            self._loop = loop
            self._on_notify_corofunc = on_notify_corofunc

        def OnNotify(self, pNotify):
            try:
                asyncio.run_coroutine_threadsafe(self._on_notify_corofunc(), self._loop)
            except Exception as e:
                if GUI_DEBUG:
                    print(f"[DEBUG] Volume callback failed: {e}")
            return 0

    def try_register_volume_callback(loop, on_notify_corofunc):
        try:
            device = AudioUtilities.GetSpeakers()
            audio_endpoint = device.EndpointVolume
            cb = AudioEndpointVolumeCallback(loop, on_notify_corofunc)
            audio_endpoint.RegisterControlChangeNotify(cb)
            if GUI_DEBUG:
                print("[DEBUG] Volume callback registrato con successo")
            return cb, audio_endpoint
        except Exception as e:
            print(f"[DEBUG] Volume callback registration failed: {e}")
            return None, None
else:
    def try_register_volume_callback(loop, on_notify_corofunc):
        return None, None

# ---------------- winsdk events registration ----------------
def schedule_from_current_factory(manager_instance, loop, send_fields_getter):
    def schedule_from_current(reason="winsdk_event"):
        try:
            sess = manager_instance.get_current_session()
            if sess:
                asyncio.run_coroutine_threadsafe(
                    gather_and_send_from_session(sess, send_fields_getter(), reason=reason),
                    loop
                )
            else:
                if GUI_DEBUG:
                    print("[DEBUG] No current session at event")
        except Exception as e:
            if GUI_DEBUG:
                print(f"[DEBUG] Schedule failed: {e}")
    return schedule_from_current

async def polling_fallback_loop(manager_instance, send_fields_getter):
    while True:
        try:
            sess = manager_instance.get_current_session() if manager_instance else None
            if sess:
                await gather_and_send_from_session(sess, send_fields_getter(), reason="poll_fallback")
        except Exception as e:
            if GUI_DEBUG:
                print(f"[DEBUG] Polling failed: {e}")
        await asyncio.sleep(POLL_FALLBACK_INTERVAL_S)

async def volume_poll_loop(send_fields_getter):
    while True:
        try:
            vol = query_system_volume_sync()
            if prev_state.get("volume") is None or abs(vol - (prev_state.get("volume") or -999)) >= VOLUME_DELTA_TO_SEND:
                if WINSKD_AVAILABLE:
                    try:
                        manager = await MediaManager.request_async()
                        sess = manager.get_current_session()
                        if sess:
                            await gather_and_send_from_session(sess, send_fields_getter(), reason="volume_poll")
                            continue
                    except Exception as e:
                        if GUI_DEBUG:
                            print(f"[DEBUG] Volume poll with session failed: {e}")
                # Fallback minimal payload
                payload = {}
                payload["volume"] = vol
                if send_fields_getter().get("ascii"):
                    payload["title_ascii"] = transliterate_basic(prev_state.get("title", ""))
                    payload["artist_ascii"] = transliterate_basic(prev_state.get("artist", ""))
                await send_payload(payload)
        except Exception as e:
            if GUI_DEBUG:
                print(f"[DEBUG] Volume poll loop error: {e}")
        await asyncio.sleep(VOLUME_POLL_INTERVAL_S)

# ---------------- main async setup ----------------
async def main_async(loop, send_fields_getter, ip_getter):
    manager_instance = None
    if WINSKD_AVAILABLE:
        try:
            manager_instance = await MediaManager.request_async()
        except Exception as e:
            print(f"[DEBUG] MediaManager request failed: {e}")

    def schedule_from_current_wrapper(reason="winsdk_event"):
        send_fields = send_fields_getter()
        try:
            if manager_instance:
                sess = manager_instance.get_current_session()
                if sess:
                    asyncio.run_coroutine_threadsafe(
                        gather_and_send_from_session(sess, send_fields, reason=reason),
                        loop
                    )
        except Exception as e:
            if GUI_DEBUG:
                print(f"[DEBUG] Schedule wrapper failed: {e}")

    if manager_instance:
        try:
            curr = manager_instance.get_current_session()
            if curr:
                curr.add_media_properties_changed(lambda s,a: schedule_from_current_wrapper("media_properties_changed"))
                curr.add_timeline_properties_changed(lambda s,a: schedule_from_current_wrapper("timeline_properties_changed"))
                curr.add_playback_info_changed(lambda s,a: schedule_from_current_wrapper("playback_info_changed"))
        except Exception as e:
            if GUI_DEBUG:
                print(f"[DEBUG] Session events attach failed: {e}")

    loop.create_task(polling_fallback_loop(manager_instance, send_fields_getter))

    if PYCW_AVAILABLE:
        try:
            cb, endpoint = try_register_volume_callback(loop, lambda: schedule_from_current_wrapper("volume_cb"))
            if not cb:
                loop.create_task(volume_poll_loop(send_fields_getter))
        except Exception as e:
            print(f"[DEBUG] Volume callback setup failed: {e}")
            loop.create_task(volume_poll_loop(send_fields_getter))
    else:
        loop.create_task(volume_poll_loop(send_fields_getter))

    while True:
        await asyncio.sleep(3600)

# ---------------- GUI (tkinter) ----------------
import tkinter as tk
from tkinter import ttk

class App:
    def __init__(self, root):
        global INCLUDE_ASCII, ESP32_IP
        self.root = root
        root.title("Media -> ESP32")

        window_width = 400
        window_height = 190
        screen_width = root.winfo_screenwidth()
        screen_height = root.winfo_screenheight()
        x = (screen_width - window_width) // 2
        y = (screen_height - window_height) // 2
        root.geometry(f"{window_width}x{window_height}+{x}+{y}")

        frame = ttk.Frame(root, padding=8)
        frame.pack(fill=tk.BOTH, expand=True)

        # IP entry
        ip_frame = ttk.Frame(frame)
        ip_frame.pack(fill=tk.X, pady=5)
        ttk.Label(ip_frame, text="ESP32 IP:").pack(side=tk.LEFT)
        self.var_ip = tk.StringVar(value=ESP32_IP)
        self.entry_ip = ttk.Entry(ip_frame, textvariable=self.var_ip, width=20)
        self.entry_ip.pack(side=tk.LEFT, padx=5)
        ttk.Label(ip_frame, text="(URL: http://[IP]/update)").pack(side=tk.LEFT, padx=5)

        # send options
        opts_frame = ttk.Labelframe(frame, text="Fields to send", padding=6)
        opts_frame.pack(fill=tk.X, pady=5)

        opts_grid = ttk.Frame(opts_frame)
        opts_grid.pack(fill=tk.X)

        self.opt_title = tk.BooleanVar(value=True)
        self.opt_artist = tk.BooleanVar(value=True)
        self.opt_status = tk.BooleanVar(value=True)
        self.opt_position = tk.BooleanVar(value=True)
        self.opt_duration = tk.BooleanVar(value=True)
        self.opt_volume = tk.BooleanVar(value=True)
        self.opt_ascii = tk.BooleanVar(value=INCLUDE_ASCII)

        ttk.Checkbutton(opts_grid, text="Title", variable=self.opt_title).grid(row=0, column=0, sticky="w", padx=5)
        ttk.Checkbutton(opts_grid, text="Artist", variable=self.opt_artist).grid(row=0, column=1, sticky="w", padx=5)
        ttk.Checkbutton(opts_grid, text="Status", variable=self.opt_status).grid(row=0, column=2, sticky="w", padx=5)
        ttk.Checkbutton(opts_grid, text="Position", variable=self.opt_position).grid(row=1, column=0, sticky="w", padx=5)
        ttk.Checkbutton(opts_grid, text="Duration", variable=self.opt_duration).grid(row=1, column=1, sticky="w", padx=5)
        ttk.Checkbutton(opts_grid, text="Volume", variable=self.opt_volume).grid(row=1, column=2, sticky="w", padx=5)
        ttk.Checkbutton(opts_grid, text="ASCII fallback", variable=self.opt_ascii, command=self.toggle_ascii).grid(row=2, column=0, sticky="w", padx=5)

        # status and buttons frame
        status_frame = ttk.Frame(frame)
        status_frame.pack(fill=tk.X, pady=5)

        self.lbl_last = ttk.Label(status_frame, text="Last send: -")
        self.lbl_last.pack(side=tk.LEFT)

        button_frame = ttk.Frame(status_frame)
        button_frame.pack(side=tk.RIGHT)

        ttk.Button(button_frame, text="Invia adesso", command=self.forced_send).pack(side=tk.LEFT, padx=5)
        ttk.Button(button_frame, text="Exit", command=self.on_exit).pack(side=tk.LEFT)

        # System tray
        self.tray_icon = None
        if TRAY_AVAILABLE:
            self.setup_tray_icon()
            root.bind("<Unmap>", self.on_minimize)
            root.protocol("WM_DELETE_WINDOW", self.on_close)
        else:
            root.protocol("WM_DELETE_WINDOW", self.on_exit)

    def setup_tray_icon(self):
        image = Image.new('RGB', (64, 64), color='white')
        dc = ImageDraw.Draw(image)
        dc.rectangle([16, 16, 48, 48], fill='blue')
        menu = pystray.Menu(
            pystray.MenuItem('Show', self.show_window),
            pystray.MenuItem('Exit', self.on_exit)
        )
        self.tray_icon = pystray.Icon("media_esp32", image, "Media ESP32", menu)
        threading.Thread(target=self.tray_icon.run, daemon=True).start()

    def on_minimize(self, event):
        if event.widget == self.root and self.root.state() == 'iconic':
            self.root.withdraw()

    def on_close(self):
        self.root.withdraw()

    def show_window(self, icon=None, item=None):
        self.root.after(0, self.root.deiconify)
        self.root.after(0, self.root.lift)

    def toggle_ascii(self):
        global INCLUDE_ASCII
        INCLUDE_ASCII = bool(self.opt_ascii.get())

    def forced_send(self):
        global ESP32_IP
        ESP32_IP = self.var_ip.get().strip()
        def do_task():
            asyncio.run_coroutine_threadsafe(self._forced_send_coroutine(), self.async_loop)
        threading.Thread(target=do_task, daemon=True).start()
        self.lbl_last['text'] = "Last send: Sending..."

    async def _forced_send_coroutine(self):
        try:
            manager = await MediaManager.request_async()
            sess = manager.get_current_session()
            send_fields = self.get_send_fields()
            if sess:
                await gather_and_send_from_session(sess, send_fields, reason="forced_send")
            else:
                payload = {}
                if send_fields.get("title"): payload["title"] = prev_state.get("title") or "Sconosciuto"
                if send_fields.get("artist"): payload["artist"] = prev_state.get("artist") or "Sconosciuto"
                if send_fields.get("status"): payload["status"] = prev_state.get("status") or "STOP"
                if send_fields.get("volume"): payload["volume"] = int(prev_state.get("volume") or 0)
                if send_fields.get("ascii"):
                    payload["title_ascii"] = transliterate_basic(payload.get("title",""))
                    payload["artist_ascii"] = transliterate_basic(payload.get("artist",""))
                await send_payload(payload)
            self.root.after(0, lambda: self.lbl_last.config(text="Last send: Success"))
        except Exception as e:
            if GUI_DEBUG:
                print(f"[DEBUG] Forced send failed: {e}")
            self.root.after(0, lambda: self.lbl_last.config(text="Last send: Failed"))

    def get_send_fields(self):
        return {
            "title": bool(self.opt_title.get()),
            "artist": bool(self.opt_artist.get()),
            "status": bool(self.opt_status.get()),
            "position": bool(self.opt_position.get()),
            "duration": bool(self.opt_duration.get()),
            "volume": bool(self.opt_volume.get()),
            "ascii": bool(self.opt_ascii.get())
        }

    def on_exit(self, icon=None, item=None):
        if self.tray_icon:
            self.tray_icon.stop()
        self.root.quit()
        self.root.destroy()

# ---------------- threads to run asyncio ----------------
def start_async_loop_in_thread(send_fields_getter, ip_getter):
    loop = asyncio.new_event_loop()
    t = threading.Thread(target=async_loop_thread, args=(loop, send_fields_getter, ip_getter), daemon=True)
    t.start()
    return loop, t

def async_loop_thread(loop, send_fields_getter, ip_getter):
    asyncio.set_event_loop(loop)
    # Inizializza COM multithreaded
    try:
        import comtypes
        comtypes.CoInitializeEx(comtypes.COINIT_MULTITHREADED)
    except Exception as e:
        print(f"[DEBUG] COM init failed: {e}")
    loop.create_task(main_async(loop, send_fields_getter, ip_getter))
    try:
        loop.run_forever()
    finally:
        if _aio_sess:
            loop.run_until_complete(_aio_sess.close())
        try:
            import comtypes
            comtypes.CoUninitialize()
        except:
            pass

# ---------------- main ----------------
def main():
    root = tk.Tk()
    app = App(root)

    def send_fields_getter():
        return app.get_send_fields()

    def ip_getter():
        return app.var_ip.get().strip()

    loop, thr = start_async_loop_in_thread(send_fields_getter, ip_getter)
    app.async_loop = loop

    root.mainloop()
    try:
        loop.call_soon_threadsafe(loop.stop)
    except:
        pass
    time.sleep(0.2)

if __name__ == "__main__":
    main()
