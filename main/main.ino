#include "arduinoFFT.h"

#define PIN_INPUT A1
#define SAMPLES 128             
#define SAMPLING_FREQUENCY 10000 

double vReal[SAMPLES];
double vImag[SAMPLES];
unsigned int sampling_period_us;

// Création de l'objet FFT
ArduinoFFT<double> FFT = ArduinoFFT<double>(vReal, vImag, SAMPLES, SAMPLING_FREQUENCY);

void setup() {
  Serial.begin(115200);
  
  // Attente USB pour l'ESP32-S2
  while (!Serial && millis() < 5000); 
  
  sampling_period_us = round(1000000 * (1.0 / SAMPLING_FREQUENCY));
  pinMode(PIN_INPUT, INPUT);
  
  Serial.println("Initialisation terminée - Mode Hertz");
}

void loop() {
  // 1. Capture des échantillons (équivalent de ton ancienne fonction extraction)
  for (int i = 0; i < SAMPLES; i++) {
    unsigned long microseconds = micros();
    
    // Lecture directe (0-4095)
    vReal[i] = analogRead(PIN_INPUT);
    vImag[i] = 0;
    
    // On force une cadence de 10kHz
    while (micros() < (microseconds + sampling_period_us));
  }

  // 2. Calculs mathématiques (FFT)
  FFT.windowing(FFT_WIN_TYP_HAMMING, FFT_FORWARD);
  FFT.compute(FFT_FORWARD);
  FFT.complexToMagnitude();

  // 3. Trouver la fréquence dominante (Le pic en Hz)
  double peak = FFT.majorPeak();

  // 4. Affichage simple pour le Traceur Série
  // On affiche le pic pour voir la valeur en continu
  Serial.print("Frequence_Hz:");
  Serial.println(peak);

  // Optionnel : ralentir un peu pour la lisibilité sur l'écran
  delay(50);
}