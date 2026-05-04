# Task 5 - TinyML Deployment & Accuracy Evaluation

## 1. Dataset description

- Dataset type: synthetic temperature and humidity samples for the DHT20 sensor pipeline.
- Total samples: 4800
- Train / test split: 3840 / 960
- Feature set: `temperature_c`, `humidity_percent`
- Class labels:
  - `normal`
  - `warning`
  - `critical`

### Data generation process

1. Generate a uniform baseline set inside the operating window:
   - temperature: 15.0 to 38.0 C
   - humidity: 20.0 to 95.0 %
2. Generate an extra boundary-focused set around the decision thresholds:
   - temperature thresholds: 20, 23, 29, 32 C
   - humidity thresholds: 30, 40, 70, 80 %
3. Add Gaussian variation around those thresholds to simulate noisy readings near class boundaries.
4. Label every sample with the same environmental rules already used by the RTOS application:
   - `critical`: temperature < 20 or >= 32, or humidity < 30 or > 80
   - `warning`: temperature < 23 or >= 29, or humidity < 40 or > 70
   - `normal`: all remaining samples

## 2. TinyML model

- Model architecture: 2 -> 10 -> 8 -> 3
- Hidden activation: ReLU
- Output: softmax probabilities for `normal`, `warning`, `critical`
- Deployment target: ESP32-S3 on Yolo UNO
- Integration method: generated weights stored in `include/tinyml_model.h`, inference runs directly inside the RTOS firmware

## 3. Accuracy evaluation

- Train accuracy: 99.06%
- Test accuracy: 99.38%
- Class accuracy:
  - normal: 97.56%
  - warning: 99.35%
  - critical: 99.65%

### Confusion matrix

Rows = ground truth, columns = predicted

| Actual \ Predicted | Normal | Warning | Critical |
| --- | ---: | ---: | ---: |
| Normal | 80 | 2 | 0 |
| Warning | 0 | 306 | 2 |
| Critical | 0 | 2 | 568 |

### On-device replay result

The same 960-sample hold-out set was replayed on the Yolo UNO after flashing the firmware.

- On-device accuracy: 99.38%
- Average inference latency: 9.86 us
- Serial evidence: `docs/task5_hardware_serial_log.txt`

## 4. Discussion

- The model reaches high accuracy because the classes are strongly correlated with temperature and humidity thresholds.
- Most mistakes happen close to boundary values between `warning` and `critical`, where small sensor variations can flip the label.
- The synthetic dataset is useful for coursework and firmware integration, but it does not replace a real deployment dataset gathered from the physical environment.
- To improve realism later, the next step would be collecting raw DHT20 readings over time and retraining with actual sensor drift and ambient noise.

## 5. Conclusion

The TinyML model is small enough to run on the Yolo UNO, preserves the older RTOS/LCD/AP/web features, and provides a deployable environmental-state classifier with test accuracy above 98%. The firmware also includes an on-device evaluation routine so the same hold-out dataset can be replayed on the microcontroller for accuracy and inference-time reporting.
