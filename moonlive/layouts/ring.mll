// A circle: `count` lights evenly around a centre, spanning 2*radius+1 cells.
// Lights and grid cells differ -- 24 lights in an 11x11 box.
// `cos`/`sin` run 0..65535 centred at 32768, so scaling by the DIAMETER lands the whole circle.

class RingLayout {
  uint8_t count = 24;
  uint8_t radius = 5;

  defineControls() {
    addUint8("count", count, 3, 255);
    addUint8("radius", radius, 1, 127);
  }

  placeLights() {
    for (i = 0; i < count; i = i + 1) {
      addLight(scale(cos(i * turn(count)), radius * 2 + 1),
               scale(sin(i * turn(count)), radius * 2 + 1), 0);
    }
  }
}
