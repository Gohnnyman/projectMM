// A diagonal run — light i at (i, i). The kind of fixture that otherwise needs its own class.

class DiagonalLayout {
  byte count = 16;

  defineControls() {
    addControl("count", count, 1, 64);
  }

  placeLights() {
    for (i = 0; i < count; i = i + 1) {
      addLight(i, i, 0);
    }
  }
}
