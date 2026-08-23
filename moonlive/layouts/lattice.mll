// A 3D lattice: stacked layers of a grid, the primitive 3D space of LED strips.
// `z` is an ordinary axis to a layout -- the shipped 2D layouts simply pass 0 for it.
// Three nested loops need more registers than Xtensa has, so this runs on P4/S31/desktop
// but not the S3; two loops (grid.mlv) fit everywhere.

class LatticeLayout {
  byte cols = 4;
  byte rows = 3;
  byte layers = 5;

  defineControls() {
    addControl("cols", cols, 1, 32);
    addControl("rows", rows, 1, 32);
    addControl("layers", layers, 1, 32);
  }

  placeLights() {
    for (z = 0; z < layers; z = z + 1) {
      for (y = 0; y < rows; y = y + 1) {
        for (x = 0; x < cols; x = x + 1) {
          addLight(x, y, z);
        }
      }
    }
  }
}
