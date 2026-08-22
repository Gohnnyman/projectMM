// A grid, the layout almost every panel is.
// `cols`/`rows` are this layout's own controls; the logical grid comes from what it places.

class GridLayout {
  uint8_t cols = 16;
  uint8_t rows = 16;

  defineControls() {
    addUint8("cols", cols, 1, 128);
    addUint8("rows", rows, 1, 128);
  }

  placeLights() {
    for (y = 0; y < rows; y = y + 1) {
      for (x = 0; x < cols; x = x + 1) {
        addLight(x, y, 0);
      }
    }
  }
}
