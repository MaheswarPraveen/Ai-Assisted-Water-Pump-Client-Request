/* ============================================================================
 *  TinyNetTest — a real neural network, hand-written in C, that TRAINS ON THE
 *  ESP32 ITSELF (forward pass + backpropagation + gradient descent, no libs).
 *
 *  Proof-of-concept: it trains from scratch on a synthetic water pattern right
 *  on the chip, then prints what it learned as an ASCII heatmap. This is the
 *  same network we'll drop into SmartPump for live on-device learning.
 *
 *  Architecture: 4 inputs -> 8 (ReLU) -> 8 (ReLU) -> 1 (sigmoid)
 *  Watch the Serial Monitor @ 115200: loss falls, then morning/evening bands
 *  appear in the heatmap.
 * ========================================================================== */

#include <math.h>
#include "esp_random.h"

// ---- network size ----
#define NI  4
#define NH1 8
#define NH2 8

// ---- weights + biases (this IS the model) ----
float W1[NI][NH1],  bias1[NH1];
float W2[NH1][NH2], bias2[NH2];
float W3[NH2],      bias3;

// ---- activations kept from the forward pass, needed by backprop ----
float Z1[NH1], A1[NH1];
float Z2[NH2], A2[NH2];
float OUT;

// ---- helpers ----
float rnd01() { return (float)esp_random() / 4294967295.0f; }          // 0..1
float rndw(float r) { return (rnd01() * 2.0f - 1.0f) * r; }            // -r..r
float relu(float x) { return x > 0 ? x : 0; }

// ---- initialise weights small & random (Xavier-ish) ----
void netInit() {
  float r1 = sqrtf(6.0f / (NI + NH1));
  float r2 = sqrtf(6.0f / (NH1 + NH2));
  float r3 = sqrtf(6.0f / (NH2 + 1));
  for (int i = 0; i < NI;  i++) for (int j = 0; j < NH1; j++) W1[i][j] = rndw(r1);
  for (int j = 0; j < NH1; j++) for (int k = 0; k < NH2; k++) W2[j][k] = rndw(r2);
  for (int k = 0; k < NH2; k++) W3[k] = rndw(r3);
  for (int j = 0; j < NH1; j++) bias1[j] = 0;
  for (int k = 0; k < NH2; k++) bias2[k] = 0;
  bias3 = 0;
}

// ---- forward pass: inputs -> probability ----
float netForward(const float x[NI]) {
  for (int j = 0; j < NH1; j++) {
    float s = bias1[j];
    for (int i = 0; i < NI; i++) s += x[i] * W1[i][j];
    Z1[j] = s; A1[j] = relu(s);
  }
  for (int k = 0; k < NH2; k++) {
    float s = bias2[k];
    for (int j = 0; j < NH1; j++) s += A1[j] * W2[j][k];
    Z2[k] = s; A2[k] = relu(s);
  }
  float s = bias3;
  for (int k = 0; k < NH2; k++) s += A2[k] * W3[k];
  OUT = 1.0f / (1.0f + expf(-s));
  return OUT;
}

// ---- one training step: forward, backprop, update. returns the loss. ----
float netTrain(const float x[NI], float target, float lr) {
  netForward(x);

  // output gradient (binary cross-entropy + sigmoid simplifies to this)
  float dz3 = OUT - target;

  // gradients flowing back to hidden layer 2
  float dZ2[NH2];
  for (int k = 0; k < NH2; k++)
    dZ2[k] = (dz3 * W3[k]) * (Z2[k] > 0 ? 1.0f : 0.0f);

  // gradients flowing back to hidden layer 1 (use OLD W2, before updating)
  float dZ1[NH1];
  for (int j = 0; j < NH1; j++) {
    float s = 0;
    for (int k = 0; k < NH2; k++) s += dZ2[k] * W2[j][k];
    dZ1[j] = s * (Z1[j] > 0 ? 1.0f : 0.0f);
  }

  // now apply all the updates (nudge every weight down its gradient)
  for (int k = 0; k < NH2; k++) W3[k] -= lr * dz3 * A2[k];
  bias3 -= lr * dz3;
  for (int j = 0; j < NH1; j++) for (int k = 0; k < NH2; k++) W2[j][k] -= lr * dZ2[k] * A1[j];
  for (int k = 0; k < NH2; k++) bias2[k] -= lr * dZ2[k];
  for (int i = 0; i < NI;  i++) for (int j = 0; j < NH1; j++) W1[i][j] -= lr * dZ1[j] * x[i];
  for (int j = 0; j < NH1; j++) bias1[j] -= lr * dZ1[j];

  // binary cross-entropy loss (just for reporting)
  return -(target * logf(OUT + 1e-7f) + (1 - target) * logf(1 - OUT + 1e-7f));
}

// ---- time -> 4 cyclical features (must match everywhere) ----
void feat(int h, int d, float x[NI]) {
  x[0] = sinf(2*PI*h/24); x[1] = cosf(2*PI*h/24);
  x[2] = sinf(2*PI*d/7);  x[3] = cosf(2*PI*d/7);
}

// ---- synthetic "ground truth" water pattern (the teacher) ----
float demand(int h, int d) {
  bool weekend = (d == 0 || d == 6);
  float p = 0.05f;
  if (weekend) p = fmaxf(p, 0.70f * expf(-powf(h - 9, 2) / 8.0f));
  else         p = fmaxf(p, 0.80f * expf(-powf(h - 7, 2) / 2.88f));
  p = fmaxf(p, 0.85f * expf(-powf(h - 20, 2) / 6.48f));
  if (h >= 10 && h <= 16) p = fmaxf(p, 0.20f);
  return fminf(p, 0.95f);
}

void printHeatmap() {
  const char* shades = " .:-=+*#%@";
  const char* days[] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
  Serial.println("\n     0         1         2   (hour)");
  Serial.println("     0123456789012345678901234");
  for (int d = 0; d < 7; d++) {
    Serial.printf("%s  ", days[d]);
    for (int h = 0; h < 24; h++) {
      float x[NI]; feat(h, d, x);
      float p = netForward(x);
      int s = (int)(p * 9.0f); if (s > 9) s = 9;
      Serial.print(shades[s]);
    }
    Serial.println();
  }
  Serial.println("(darker/@ = pump more likely, space = unlikely)\n");
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n=== TinyNet: training a neural net on the ESP32 itself ===");

  netInit();
  Serial.println("Before training (random weights):");
  printHeatmap();

  const int STEPS = 60000;
  float lr = 0.03f, running = 0;
  uint32_t t0 = millis();
  for (int step = 1; step <= STEPS; step++) {
    int h = esp_random() % 24;
    int d = esp_random() % 7;
    float x[NI]; feat(h, d, x);
    float target = (rnd01() < demand(h, d)) ? 1.0f : 0.0f;   // Bernoulli label
    running += netTrain(x, target, lr);
    if (step % 6000 == 0) {
      Serial.printf("step %6d   avg loss %.4f\n", step, running / 6000.0f);
      running = 0;
    }
  }
  Serial.printf("Trained %d steps in %lu ms\n", STEPS, millis() - t0);

  Serial.println("\nAfter training — what the network learned:");
  printHeatmap();
  Serial.println("Compare to reality (morning ~7, evening ~20, quiet nights).");
}

void loop() {}
