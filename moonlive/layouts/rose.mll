// Rose: the strand traces a rhodonea curve, a circle whose radius swells and collapses
// `petals` times per revolution, drawing a flower. The classic polar curve r = sin(k * a),
// built from the layout vocabulary alone: turn(n) steps the angle, sin(a * petals) is the
// petal envelope, and the biased-unsigned trick from the effect docs
// (scale(cos(a), 2 * r + 1) sweeps the whole diameter) centers each axis.
//
// The envelope is recomputed where it is used: the grammar has no locals, and a layout
// walk runs once per edit, so clarity beats the repeated call.

class RoseLayout {
  byte petals = 2;
  byte radius = 15;

  void defineControls() {
    addControl("petals", petals, 1, 8);
    addControl("radius", radius, 4, 30);
  }

  void placeLights() {
    for (i = 0; i < 256; i = i + 1) {
      addLight(radius - scale(sin(i * turn(256) * petals), radius + 1)
                 + scale(cos(i * turn(256)),
                         2 * scale(sin(i * turn(256) * petals), radius + 1) + 1),
               radius - scale(sin(i * turn(256) * petals), radius + 1)
                 + scale(sin(i * turn(256)),
                         2 * scale(sin(i * turn(256) * petals), radius + 1) + 1),
               0);
    }
  }
}
