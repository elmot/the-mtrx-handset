#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-only
# Sxmo deviceprofile for Nokia 8110 4G (nokia,argon)
# 240x320 QVGA portrait SPI panel, physical T9 keypad + D-pad + slider + headset jack

export SXMO_MONITOR="SPI-1"
export SXMO_SWAY_SCALE="1"
export SXMO_POWER_BUTTON="0:0:pm8941_pwrkey"
export SXMO_VIBRATE_DEV="/dev/input/by-path/platform-200f000.spmi-platform-200f000.spmi:pmic@1:vibrator@c000-event"
export SXMO_VIBRATE_STRENGTH="20000"
export SXMO_MIN_BRIGHTNESS="1"
export SXMO_NO_VIRTUAL_KEYBOARD="1"
export SXMO_DISABLE_LEDS="1"

# Fit menus comfortably on 240x320 portrait screen
export SXMO_BEMENU_PORTRAIT_LINES="10"
export SXMO_BEMENU_LANDSCAPE_LINES="10"
export SXMO_DMENU_PORTRAIT_LINES="10"
export SXMO_DMENU_LANDSCAPE_LINES="10"
export BEMENU_OPTS='--ab "#222222" --af "#bbbbbb" --bdr "#005577" --border 2 --cb "#222222" --center --cf "#bbbbbb" --fb "#222222" --fbb "#eeeeee" --fbf "#222222" --ff "#bbbbbb" --fixed-height --fn "Sxmo 9" --hb "#005577" --hf "#eeeeee" --line-height 16 --list 10 --margin 4 --nb "#222222" --nf "#bbbbbb" --no-overlap --no-spacing --sb "#323232" --scb "#005577" --scf "#eeeeee" --scrollbar autohide --tb "#005577" --tf "#eeeeee" --wrap'
