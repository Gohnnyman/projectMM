// A strand wired right to left: light 0 sits at the far end.

class ReversedRowLayout {
  uint8_t cols = 16;

  defineControls() {
    addUint8("cols", cols, 1, 64);
  }

  placeLights() {
    for (i = 0; i < cols; i = i + 1) {
      addLight(cols - 1 - i, 0, 0);
    }
  }
}
