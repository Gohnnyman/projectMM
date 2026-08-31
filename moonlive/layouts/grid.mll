// A grid, the layout almost every panel is.
// `cols`/`rows` are this layout's own controls; the logical grid comes from what it places.

class GridLayout {
  byte cols = 16;
  byte rows = 16;

  void defineControls() {
    addControl("cols", cols, 1, 128);
    addControl("rows", rows, 1, 128);
  }

  void placeLights() {
    for (y = 0; y < rows; y = y + 1) {
      for (x = 0; x < cols; x = x + 1) {
        addLight(x, y, 0);
      }
    }
  }
}
