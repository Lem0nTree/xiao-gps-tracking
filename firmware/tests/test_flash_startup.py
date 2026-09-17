#!/usr/bin/env python3
"""Check actual boot wake code with GPIO edges and a powered flash model.

Requires g++. This checks command framing/order, not physical QSPI hardware.
"""
from pathlib import Path
import re
import subprocess
import tempfile

source = (Path(__file__).resolve().parents[1] /
          'XiaoGpsTracker/XiaoGpsTracker.ino').read_text()


def function(name):
    match = re.search(r'^(?:void|bool) ' + name + r'\([^\n]*\) \{', source, re.M)
    assert match, name
    end = source.index('{', match.start()) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[match.start():end]


harness = r'''
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <algorithm>
#include <vector>
constexpr int LOW=0, HIGH=1, INPUT=0, OUTPUT=1;
constexpr int PIN_QSPI_CS=0, PIN_QSPI_SCK=1, PIN_QSPI_IO0=2;
constexpr int PIN_QSPI_IO1=3, PIN_QSPI_IO2=4, PIN_QSPI_IO3=5;
int levels[6], modes[6], bitCount, beginCalls;
uint8_t opcode;
uint32_t nowUs, releasedAt, beginAt;
bool selected, sleeping, present, qspiOn, flashSleeping;
std::vector<uint8_t> frames;
void pinMode(int pin, int mode) { assert(!qspiOn); modes[pin]=mode; }
void delayMicroseconds(uint32_t us) { nowUs += us; }
void digitalWrite(int pin, int value) {
  assert(!qspiOn); // GPIO wake must precede peripheral ownership.
  const int old=levels[pin];
  if (pin==PIN_QSPI_CS && old==HIGH && value==LOW) {
    assert(modes[PIN_QSPI_CS]==OUTPUT && levels[PIN_QSPI_SCK]==LOW);
    selected=true; bitCount=0; opcode=0;
  }
  if (selected && pin==PIN_QSPI_SCK && old==LOW && value==HIGH) {
    assert(modes[PIN_QSPI_SCK]==OUTPUT && modes[PIN_QSPI_IO0]==OUTPUT);
    assert(modes[PIN_QSPI_IO1]==INPUT);
    assert(modes[PIN_QSPI_IO2]==OUTPUT && levels[PIN_QSPI_IO2]==HIGH);
    assert(modes[PIN_QSPI_IO3]==OUTPUT && levels[PIN_QSPI_IO3]==HIGH);
    opcode=uint8_t((opcode<<1)|levels[PIN_QSPI_IO0]); ++bitCount;
  }
  if (selected && pin==PIN_QSPI_CS && value==HIGH) {
    assert(bitCount==8 && levels[PIN_QSPI_SCK]==LOW);
    frames.push_back(opcode); selected=false; releasedAt=nowUs;
    if (present && opcode==0xAB) sleeping=false;
  }
  levels[pin]=value;
}
int flashDevices[1]={0};
struct Flash {
  // No erase/write API: any new destructive operation fails to compile.
  bool begin(const int*, int count) {
    assert(count==1 && !qspiOn); qspiOn=true; ++beginCalls; beginAt=nowUs;
    return present && !sleeping && (frames.empty() || nowUs-releasedAt>=8);
  }
} flash;
void resetCpu(bool asleep, bool attached=true) {
  std::fill(levels, levels+6, LOW); std::fill(modes, modes+6, INPUT);
  selected=qspiOn=false; sleeping=asleep; present=attached;
  flashSleeping=false; // CPU RAM forgets the externally powered chip's state.
  nowUs=releasedAt=beginAt=0; beginCalls=bitCount=0; frames.clear();
}
'''
harness += function('wakeFlashBeforeInit') + '\n' + function('initializeFlash')
harness += r'''
void checkWake(bool result) {
  assert(initializeFlash()==result);
  assert(frames==std::vector<uint8_t>{0xAB});
  assert(beginCalls==1 && beginAt-releasedAt>=50);
  assert(!selected && levels[PIN_QSPI_CS]==HIGH && levels[PIN_QSPI_SCK]==LOW);
  assert(!flashSleeping);
}
int main() {
  resetCpu(true);
  assert(!flash.begin(flashDevices,1)); // Original boot fails with sleeping chip.
  resetCpu(true); checkWake(true);
  resetCpu(false); checkWake(true);
  for (int i=0;i<5;++i) { resetCpu(true); checkWake(true); }
  resetCpu(true,false); checkWake(false);
  puts("PASS warm-reset regression, cold boot, repeated resets, absent flash");
  puts("PASS mode-0 0xAB framing, pin directions, recovery delay before QSPI init");
}
'''

with tempfile.TemporaryDirectory(prefix='xiao-flash-startup-') as temp:
    cpp = Path(temp) / 'test.cpp'
    exe = Path(temp) / 'test'
    cpp.write_text(harness)
    subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                    str(cpp), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
