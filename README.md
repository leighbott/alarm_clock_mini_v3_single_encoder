# Alarm Clock Mini v2

A sophisticated bedside alarm clock built on ESP32 with a fully customizable LVGL UI, dual LED strips, environmental sensing, and hardware rotary encoder + button navigation.

## Overview

This is a feature-rich alarm clock designed for users who want precise control over their wake-up experience. Rather than relying on touch gestures, the device uses a tactile rotary encoder interface for accessible, distraction-free interaction. The UI runs entirely on LVGL and is optimized for a compact display with no scrolling.

## Key Features

### 🎨 Customizable Home Screen
- **Persistent storage**: All customizations saved to NVS (non-volatile storage)

### ⏰ Advanced Alarm System
- **Sunrise simulation**: Gradual LED + audio ramp-up over user-configured duration
- **Flexible scheduling**:
  - One-time alarms or repeat by day-of-week
  - Configurable alarm start time and sound
- **Quick alarm menu**: Fast access from home screen to set hour/minute without navigating full settings
- **Snooze support**: Dismiss and delay alarm with user-defined intervals
- **Auto Loud and Auto Dismiss**: Optionally raise both LED strips and volume after the alarm becomes active, then dismiss after a further configured delay
- **Alarm countdown**: Home screen shows hours remaining for alarms under 12 hours away and a tomorrow notice for alarms within 24 hours

### 💡 Dual Independent LED Strips
- **Two WS2812B RGB LED strips** (front and rear) with independent brightness/color control
- **Sunrise ramp**: Both strips gradually brighten during alarm wake-up
- **Manual control**: Adjust front LED from home screen; full control in settings
- **Auto LED Off**: After the configurable inactivity interval (10 minutes to 6 hours), both strips fade to off over one second
- **Persistence**: LED color/brightness preferences saved across power cycles
- Uses LiteLED library with dedicated RMT channels for stable operation

### 🔊 Audio System
- **Configurable beep presets**: Multiple alarm tones available
- **Playback safeguards**: Prevents audio crashes when display SPI bus is active
- **Audio preview**: Pre-listen to alarm tones before saving

### 📊 Environmental Monitoring
- **Temperature, humidity, and pressure sensing** displayed on home screen
- **Auto-brightness display**: Backlight brightness adjusts based on ambient light (LDR sensor)
- **Brightness boost**: User activity boosts the display without dimming below a higher ambient auto-brightness level
- **Manual brightness control**: User-selectable minimum/maximum levels and boost duration

### ⏱️ Real-Time Clock (RTC)
- **Date and time management** with roller-selector interface
- **Day-of-week calculation** displayed in customizable home element
- **Persistent storage**: Maintains accurate time across power loss (with battery backup)

### 🎛️ Hardware Interface
- **Two rotary encoders** (ENC1/ENC2) with integrated buttons
  - ENC1: Navigate menus and confirm
  - ENC2: Adjust values, open quick settings
- **No touch, no swipes**: Pure physical interaction

NV3007 wide bar display