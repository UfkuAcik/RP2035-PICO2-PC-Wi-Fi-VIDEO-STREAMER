import customtkinter as ctk
from tkinter import filedialog, messagebox
import socket
import struct
import subprocess
import time
import threading
import ctypes
import os

try:
    # Set Windows OS timer resolution to 1ms for highly accurate time.sleep()
    if os.name == 'nt':
        ctypes.windll.winmm.timeBeginPeriod(1)
except Exception:
    pass

# Settings (Monochrome Theme)
ctk.set_appearance_mode("Dark")
ctk.set_default_color_theme("dark-blue") # Will be overridden by custom gray colors

class PicoCastPlayer(ctk.CTk):
    def __init__(self):
        super().__init__()
        self.title("Pico-Cast Media Player")
        self.geometry("600x420")
        self.resizable(False, False)
        
        # State Variables
        self.is_connected = False
        self.is_streaming = False
        self.is_paused = False
        self.speed = 1.0
        self.video_duration = 0
        self.current_time = 0
        self.is_dragging = False
        self.video_seek_requested = False
        self.audio_seek_requested = False
        self.seek_target = 0.0
        
        self.client_socket = None
        self.audio_socket = None
        self.process = None
        self.audio_process = None
        
        # Custom monochrome colors (Strictly Gray/Black/White)
        self.bg_color = "#121212"
        self.frame_color = "#1E1E1E"
        self.entry_color = "#2A2A2A"
        self.btn_color = "#333333"
        self.btn_hover = "#444444"
        self.text_color = "#E0E0E0"
        
        self.configure(fg_color=self.bg_color)
        
        self._build_ui()
        self.update_ui_timer()
        
    def _build_ui(self):
        # 1. Top Section: IP and File Selection
        top_frame = ctk.CTkFrame(self, fg_color=self.frame_color)
        top_frame.pack(pady=20, padx=20, fill="x")
        
        ctk.CTkLabel(top_frame, text="Pico IP:", font=("Inter", 14, "bold"), text_color=self.text_color).grid(row=0, column=0, padx=10, pady=10, sticky="e")
        self.ip_entry = ctk.CTkEntry(top_frame, width=170, placeholder_text="192.168.1.100", fg_color=self.entry_color, text_color=self.text_color, border_color=self.btn_hover)
        self.ip_entry.grid(row=0, column=1, padx=(10, 5), pady=10)
        
        self.connect_btn = ctk.CTkButton(top_frame, text="Connect", width=70, fg_color=self.btn_color, hover_color=self.btn_hover, text_color=self.text_color, command=self.connect_action)
        self.connect_btn.grid(row=0, column=2, padx=(5, 10), pady=10)
        
        ctk.CTkLabel(top_frame, text="Video:", font=("Inter", 14, "bold"), text_color=self.text_color).grid(row=1, column=0, padx=10, pady=10, sticky="e")
        self.file_entry = ctk.CTkEntry(top_frame, width=170, placeholder_text="Select a video file...", fg_color=self.entry_color, text_color=self.text_color, border_color=self.btn_hover, state="disabled")
        self.file_entry.grid(row=1, column=1, padx=(10, 5), pady=10)
        
        self.browse_btn = ctk.CTkButton(top_frame, text="Browse", width=70, fg_color=self.btn_color, hover_color=self.btn_hover, text_color=self.text_color, command=self.browse_file, state="disabled")
        self.browse_btn.grid(row=1, column=2, padx=(5, 10), pady=10)
        
        # 2. Middle Section: Slider
        mid_frame = ctk.CTkFrame(self, fg_color=self.frame_color)
        mid_frame.pack(pady=10, padx=20, fill="x")
        
        self.time_label = ctk.CTkLabel(mid_frame, text="00:00 / 00:00", font=("Inter", 14), text_color=self.text_color)
        self.time_label.pack(pady=(10, 0))
        
        self.slider = ctk.CTkSlider(mid_frame, from_=0, to=100, command=self.slider_dragged, button_color="#555555", button_hover_color="#777777", progress_color="#666666")
        self.slider.set(0)
        self.slider.bind("<ButtonRelease-1>", self.slider_released)
        self.slider.pack(pady=15, padx=20, fill="x")
        
        # 3. Bottom Section: Media Controls
        bot_frame = ctk.CTkFrame(self, fg_color="transparent")
        bot_frame.pack(pady=10, padx=20, fill="x")
        
        self.play_btn = ctk.CTkButton(bot_frame, text="▶ Play", width=90, font=("Inter", 14, "bold"), fg_color=self.btn_color, hover_color=self.btn_hover, text_color=self.text_color, command=self.play_action, state="disabled")
        self.play_btn.pack(side="left", padx=10)
        
        self.pause_btn = ctk.CTkButton(bot_frame, text="⏸ Pause", width=90, font=("Inter", 14, "bold"), fg_color=self.btn_color, hover_color=self.btn_hover, text_color=self.text_color, command=self.pause_action, state="disabled")
        self.pause_btn.pack(side="left", padx=10)
        
        self.stop_btn = ctk.CTkButton(bot_frame, text="⏹ Stop", width=90, font=("Inter", 14, "bold"), fg_color=self.btn_color, hover_color=self.btn_hover, text_color=self.text_color, command=self.stop_action, state="disabled")
        self.stop_btn.pack(side="left", padx=10)
        
        ctk.CTkLabel(bot_frame, text="Speed:", font=("Inter", 14), text_color=self.text_color).pack(side="left", padx=(30, 5))
        self.speed_menu = ctk.CTkOptionMenu(bot_frame, values=["0.5x", "1.0x", "1.5x", "2.0x"], width=80, fg_color=self.btn_color, button_color=self.btn_color, button_hover_color=self.btn_hover, text_color=self.text_color, command=self.change_speed, state="disabled")
        self.speed_menu.set("1.0x")
        self.speed_menu.pack(side="left", padx=5)
        
        # 4. Status Label
        self.status_label = ctk.CTkLabel(self, text="Status: Waiting for connection...", text_color="#777777", font=("Inter", 12))
        self.status_label.pack(side="bottom", pady=10)

    def connect_action(self):
        ip = self.ip_entry.get().strip()
        if not ip:
            messagebox.showerror("Error", "Please enter Pico IP address.")
            return
            
        self.is_connected = True
        self.ip_entry.configure(state="disabled")
        self.connect_btn.configure(text="Connected", state="disabled")
        self.file_entry.configure(state="normal")
        self.browse_btn.configure(state="normal")
        self.status_label.configure(text="Status: Connected to Pico")

    def browse_file(self):
        filepath = filedialog.askopenfilename(filetypes=[("Video Files", "*.mp4 *.mkv *.avi *.webm")])
        if filepath:
            self.file_entry.delete(0, ctk.END)
            self.file_entry.insert(0, filepath)
            self.play_btn.configure(state="normal")
            
    def get_video_duration(self, filepath):
        try:
            cmd = ['ffprobe', '-v', 'error', '-show_entries', 'format=duration', '-of', 'default=noprint_wrappers=1:nokey=1', filepath]
            out = subprocess.check_output(cmd, stderr=subprocess.STDOUT, creationflags=subprocess.CREATE_NO_WINDOW)
            return float(out.decode().strip())
        except:
            return 100.0 # Default fallback if error

    def format_time(self, seconds):
        m, s = divmod(int(seconds), 60)
        h, m = divmod(m, 60)
        if h > 0:
            return f"{h:02d}:{m:02d}:{s:02d}"
        return f"{m:02d}:{s:02d}"

    def update_ui_timer(self):
        # Update UI timer every 100ms based on current time
        if self.is_streaming and not self.video_seek_requested and not getattr(self, 'is_dragging', False):
            self.time_label.configure(text=f"{self.format_time(self.current_time)} / {self.format_time(self.video_duration)}")
            if not self.is_paused:
                self.slider.set(self.current_time)
        self.after(100, self.update_ui_timer)

    def slider_dragged(self, value):
        self.is_dragging = True
        self.time_label.configure(text=f"{self.format_time(float(value))} / {self.format_time(self.video_duration)}")
        if not self.is_streaming:
            self.current_time = float(value)
        
    def slider_released(self, event):
        if self.is_streaming:
            self.seek_target = self.slider.get()
            self.video_seek_requested = True
            self.audio_seek_requested = True
            self.status_label.configure(text="Status: Seeking...")
        self.is_dragging = False

    def change_speed(self, choice):
        self.speed = float(choice.replace("x", ""))
        if self.is_streaming:
            self.seek_target = self.current_time
            self.video_seek_requested = True
            self.audio_seek_requested = True
        self.status_label.configure(text=f"Status: Speed {self.speed}x")

    def play_action(self):
        if self.is_paused:
            # Resume if paused
            self.is_paused = False
            self.status_label.configure(text="Status: Playing")
            self.play_btn.configure(state="disabled")
            self.pause_btn.configure(state="normal")
            return
            
        ip = self.ip_entry.get().strip()
        video_path = self.file_entry.get().strip()

        if not ip or not video_path:
            messagebox.showerror("Error", "Please enter IP address and select a video file.")
            return

        self.video_duration = self.get_video_duration(video_path)
        self.slider.configure(to=self.video_duration)
        self.slider.set(0)
        self.current_time = 0.0
        
        self.is_streaming = True
        self.is_paused = False
        self.video_seek_requested = False
        self.audio_seek_requested = False
        
        self.play_btn.configure(state="disabled")
        self.pause_btn.configure(state="normal")
        self.stop_btn.configure(state="normal")
        self.speed_menu.configure(state="normal")
        self.status_label.configure(text="Status: Playing")

        # Start threads
        threading.Thread(target=self.video_stream_thread, args=(ip, video_path), daemon=True).start()
        threading.Thread(target=self.audio_stream_thread, args=(ip, video_path), daemon=True).start()

    def pause_action(self):
        if self.is_streaming and not self.is_paused:
            self.is_paused = True
            self.status_label.configure(text="Status: Paused")
            self.play_btn.configure(state="normal")
            self.pause_btn.configure(state="disabled")

    def stop_action(self):
        self.is_streaming = False
        self.is_paused = False
        if self.process and self.process.poll() is None:
            self.process.terminate()
        if self.audio_process and self.audio_process.poll() is None:
            self.audio_process.terminate()
            
        self.play_btn.configure(state="normal")
        self.pause_btn.configure(state="disabled")
        self.stop_btn.configure(state="disabled")
        self.speed_menu.configure(state="disabled")
        self.status_label.configure(text="Status: Stopped")
        self.current_time = 0.0
        self.slider.set(0)
        self.time_label.configure(text=f"00:00 / {self.format_time(self.video_duration)}")



    def video_stream_thread(self, ip, video_path):
        port = 8080
        self.client_socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.client_socket.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 1024 * 1024)
        
        frame_id = 0
        start_offset = 0.0
        fps = 60.0
        
        while self.is_streaming:
            if self.video_seek_requested:
                if self.process:
                    try:
                        self.process.terminate()
                        if self.process.stdout:
                            self.process.stdout.close()
                        self.process.wait(timeout=1.0)
                    except:
                        try:
                            self.process.kill()
                        except:
                            pass
                start_offset = self.seek_target
                self.current_time = start_offset
                self.video_seek_requested = False
                
            if self.speed == 1.0:
                v_filter = "scale=320:240:flags=lanczos,eq=contrast=1.05:saturation=1.1,unsharp=5:5:1.5:5:5:0.0"
            else:
                v_filter = f"scale=320:240:flags=lanczos,eq=contrast=1.05:saturation=1.1,unsharp=5:5:1.5:5:5:0.0,setpts={1.0/self.speed}*PTS"
                
            cmd = [
                'ffmpeg', '-hwaccel', 'auto', '-ss', str(start_offset), '-i', video_path,
                '-threads', '0', '-r', str(fps),
                '-vf', v_filter,
                '-sws_dither', 'bayer',
                '-c:v', 'mjpeg', '-q:v', '6',
                '-pix_fmt', 'yuvj420p', '-color_range', '2',
                '-f', 'image2pipe', '-'
            ]
            
            try:
                self.process = subprocess.Popen(cmd, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, creationflags=subprocess.CREATE_NO_WINDOW)
            except Exception as e:
                print(f"Error starting video ffmpeg: {e}")
                time.sleep(0.5)
                continue
                
            buffer = b''
            start_time_vid = time.perf_counter()
            frames_sent = 0
            
            while self.is_streaming and not self.video_seek_requested:
                if self.is_paused:
                    time.sleep(0.05)
                    start_time_vid = time.perf_counter() - (frames_sent / fps)
                    continue
                    
                chunk = self.process.stdout.read(8192)
                if not chunk:
                    break
                buffer += chunk
                
                while True:
                    start = buffer.find(b'\xff\xd8')
                    if start == -1:
                        buffer = buffer[-1:] if buffer else b''
                        break
                    
                    end = buffer.find(b'\xff\xd9', start + 2)
                    if end == -1:
                        buffer = buffer[start:]
                        break
                        
                    jpeg_data = buffer[start:end+2]
                    buffer = buffer[end+2:]
                    
                    if self.video_seek_requested:
                        break
                        
                    if len(jpeg_data) > 20000:
                        continue # Drop frame in Python if it exceeds Pico hardware limit
                        
                    frame_id = (frame_id + 1) & 0xFFFFFFFF
                    self.current_time += (1.0 / fps) * self.speed
                    frames_sent += 1
                    
                    # Absolute Sleep Pacing (CPU Friendly & Drift-Free)
                    sleep_time = frames_sent / fps
                    target = start_time_vid + sleep_time
                    
                    wait_time = target - time.perf_counter()
                    if wait_time > 0.001: # OS sleep for bulk
                        time.sleep(wait_time - 0.0005) # Sleep until 0.5ms before
                        
                    while time.perf_counter() < target: # Busy wait for microsecond precision
                        pass
                        
                    chunk_size = 1400
                    total_chunks = (len(jpeg_data) + chunk_size - 1) // chunk_size
                    
                    for i in range(total_chunks):
                        c = jpeg_data[i*chunk_size : (i+1)*chunk_size]
                        header = struct.pack('<IHH', frame_id, i, total_chunks)
                        packet = header + c
                        try:
                            self.client_socket.sendto(packet, (ip, port))
                        except Exception as e:
                            self.is_streaming = False
                            break
                        
            if not self.video_seek_requested and self.is_streaming:
                self.after(0, self.stop_action)
                break

    def audio_stream_thread(self, ip, video_path):
        port = 8081
        self.audio_socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        
        start_offset = 0.0
        
        while self.is_streaming:
            if self.audio_seek_requested:
                if self.audio_process:
                    try:
                        self.audio_process.terminate()
                        if self.audio_process.stdout:
                            self.audio_process.stdout.close()
                        self.audio_process.wait(timeout=1.0)
                    except:
                        try:
                            self.audio_process.kill()
                        except:
                            pass
                start_offset = self.seek_target
                self.audio_seek_requested = False
                
            if self.speed == 1.0:
                a_filter = "volume=0.25"
            else:
                a_filter = f"atempo={self.speed},volume=0.25"
                
            cmd = [
                'ffmpeg', '-ss', str(start_offset), '-i', video_path,
                '-vn', '-acodec', 'pcm_s16le', '-ar', '44100', '-ac', '2',
                '-af', a_filter,
                '-f', 's16le', '-'
            ]
            
            try:
                self.audio_process = subprocess.Popen(cmd, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, creationflags=subprocess.CREATE_NO_WINDOW)
            except Exception as e:
                print(f"Error starting audio ffmpeg: {e}")
                time.sleep(0.5)
                continue
            
            start_time = time.perf_counter()
            bytes_sent = 0
            last_send_time = time.perf_counter()
            
            while self.is_streaming and not self.audio_seek_requested:
                if self.is_paused:
                    time.sleep(0.05)
                    # Shift start time forward so we don't send a massive burst of audio when unpaused
                    start_time = time.perf_counter() - (bytes_sent / 176400.0)
                    last_send_time = time.perf_counter()
                    continue
                    
                # Read 1024 bytes = 256 samples (stereo, 16-bit)
                chunk = self.audio_process.stdout.read(1024)
                if not chunk:
                    break
                    
                # Enforce minimum gap of 2ms to prevent UDP packet drops during ffmpeg filter bursts
                min_gap_target = last_send_time + 0.002
                gap_wait = min_gap_target - time.perf_counter()
                if gap_wait > 0.001:
                    time.sleep(gap_wait - 0.0005)
                while time.perf_counter() < min_gap_target:
                    pass
                
                self.audio_socket.sendto(chunk, (ip, 8081))
                bytes_sent += len(chunk)
                last_send_time = time.perf_counter()
                
                # Absolute Sleep Pacing (Drift-Free)
                sleep_time = (bytes_sent / 176400.0)
                target = start_time + sleep_time
                
                wait_time = target - time.perf_counter()
                if wait_time > 0.001: # OS sleep for bulk
                    time.sleep(wait_time - 0.0005)
                    
                while time.perf_counter() < target: # Busy wait for microsecond precision
                    pass
                    
            if not self.audio_seek_requested and self.is_streaming:
                break

if __name__ == '__main__':
    app = PicoCastPlayer()
    app.mainloop()
