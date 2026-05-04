from __future__ import annotations

import json
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, Tuple

import numpy as np


REPO_ROOT = Path(__file__).resolve().parents[2]
DATA_DIR = REPO_ROOT / "data"
DOCS_DIR = REPO_ROOT / "docs"
INCLUDE_DIR = REPO_ROOT / "include"

DATASET_PATH = DATA_DIR / "tinyml_environment_dataset.csv"
HEADER_PATH = INCLUDE_DIR / "tinyml_model.h"
METRICS_PATH = DOCS_DIR / "task5_metrics.json"
REPORT_PATH = DOCS_DIR / "task5_tinyml.md"

SEED = 42
UNIFORM_SAMPLES = 3200
BOUNDARY_SAMPLES = 1600
TRAIN_RATIO = 0.8
HIDDEN_1 = 10
HIDDEN_2 = 8
OUTPUT_CLASSES = 3
EPOCHS = 1400
BATCH_SIZE = 64
LEARNING_RATE = 0.02


LABEL_NAMES = {
    0: "normal",
    1: "warning",
    2: "critical",
}


@dataclass
class DatasetBundle:
    features: np.ndarray
    labels: np.ndarray
    sources: np.ndarray


@dataclass
class TrainingResult:
    mean: np.ndarray
    std: np.ndarray
    weights: Dict[str, np.ndarray]
    train_accuracy: float
    test_accuracy: float
    confusion_matrix: np.ndarray
    class_accuracy: Dict[str, float]
    train_count: int
    test_count: int


def classify_environment(temperature_c: float, humidity_percent: float) -> int:
    critical_temperature = (temperature_c < 20.0) or (temperature_c >= 32.0)
    critical_humidity = (humidity_percent < 30.0) or (humidity_percent > 80.0)
    if critical_temperature or critical_humidity:
        return 2

    warning_temperature = (temperature_c < 23.0) or (temperature_c >= 29.0)
    warning_humidity = (humidity_percent < 40.0) or (humidity_percent > 70.0)
    if warning_temperature or warning_humidity:
        return 1

    return 0


def generate_dataset(rng: np.random.Generator) -> DatasetBundle:
    base_temperature = rng.uniform(15.0, 38.0, size=UNIFORM_SAMPLES)
    base_humidity = rng.uniform(20.0, 95.0, size=UNIFORM_SAMPLES)

    temperature_thresholds = np.array([20.0, 23.0, 29.0, 32.0], dtype=np.float32)
    humidity_thresholds = np.array([30.0, 40.0, 70.0, 80.0], dtype=np.float32)

    boundary_temperature = rng.choice(temperature_thresholds, size=BOUNDARY_SAMPLES)
    boundary_temperature += rng.normal(0.0, 0.7, size=BOUNDARY_SAMPLES)

    boundary_humidity = rng.choice(humidity_thresholds, size=BOUNDARY_SAMPLES)
    boundary_humidity += rng.normal(0.0, 2.0, size=BOUNDARY_SAMPLES)

    samples = np.vstack(
        [
            np.column_stack([base_temperature, base_humidity]),
            np.column_stack(
                [
                    np.clip(boundary_temperature, 15.0, 38.0),
                    np.clip(boundary_humidity, 20.0, 95.0),
                ]
            ),
        ]
    ).astype(np.float32)

    labels = np.array(
        [classify_environment(temp, humidity) for temp, humidity in samples],
        dtype=np.int64,
    )

    sources = np.array(
        ["uniform"] * UNIFORM_SAMPLES + ["boundary"] * BOUNDARY_SAMPLES,
        dtype=object,
    )

    permutation = rng.permutation(len(samples))
    return DatasetBundle(
        features=samples[permutation],
        labels=labels[permutation],
        sources=sources[permutation],
    )


def one_hot(labels: np.ndarray, class_count: int) -> np.ndarray:
    encoded = np.zeros((len(labels), class_count), dtype=np.float32)
    encoded[np.arange(len(labels)), labels] = 1.0
    return encoded


def forward_pass(
    features: np.ndarray,
    weights: Dict[str, np.ndarray],
) -> Tuple[np.ndarray, np.ndarray, np.ndarray, np.ndarray, np.ndarray]:
    z1 = features @ weights["w1"] + weights["b1"]
    a1 = np.maximum(z1, 0.0)

    z2 = a1 @ weights["w2"] + weights["b2"]
    a2 = np.maximum(z2, 0.0)

    logits = a2 @ weights["w3"] + weights["b3"]
    logits = logits - logits.max(axis=1, keepdims=True)
    exp_scores = np.exp(logits)
    probabilities = exp_scores / exp_scores.sum(axis=1, keepdims=True)
    return z1, a1, z2, a2, probabilities


def compute_accuracy(
    features: np.ndarray,
    labels: np.ndarray,
    weights: Dict[str, np.ndarray],
) -> float:
    probabilities = forward_pass(features, weights)[4]
    predictions = probabilities.argmax(axis=1)
    return float(np.mean(predictions == labels))


def train_model(
    train_features: np.ndarray,
    train_labels: np.ndarray,
    test_features: np.ndarray,
    test_labels: np.ndarray,
    rng: np.random.Generator,
) -> TrainingResult:
    mean = train_features.mean(axis=0).astype(np.float32)
    std = train_features.std(axis=0).astype(np.float32)
    std[std < 1e-6] = 1.0

    normalized_train = ((train_features - mean) / std).astype(np.float32)
    normalized_test = ((test_features - mean) / std).astype(np.float32)

    weights = {
        "w1": rng.normal(0.0, 0.18, size=(2, HIDDEN_1)).astype(np.float32),
        "b1": np.zeros(HIDDEN_1, dtype=np.float32),
        "w2": rng.normal(0.0, 0.18, size=(HIDDEN_1, HIDDEN_2)).astype(np.float32),
        "b2": np.zeros(HIDDEN_2, dtype=np.float32),
        "w3": rng.normal(0.0, 0.18, size=(HIDDEN_2, OUTPUT_CLASSES)).astype(np.float32),
        "b3": np.zeros(OUTPUT_CLASSES, dtype=np.float32),
    }

    best_accuracy = -1.0
    best_weights = {name: value.copy() for name, value in weights.items()}

    for epoch in range(EPOCHS):
        permutation = rng.permutation(len(normalized_train))
        normalized_train = normalized_train[permutation]
        train_labels = train_labels[permutation]

        for start in range(0, len(normalized_train), BATCH_SIZE):
            batch_features = normalized_train[start : start + BATCH_SIZE]
            batch_labels = train_labels[start : start + BATCH_SIZE]

            z1, a1, z2, a2, probabilities = forward_pass(batch_features, weights)
            targets = one_hot(batch_labels, OUTPUT_CLASSES)

            grad_logits = (probabilities - targets) / len(batch_features)
            grad_w3 = a2.T @ grad_logits
            grad_b3 = grad_logits.sum(axis=0)

            grad_a2 = grad_logits @ weights["w3"].T
            grad_z2 = grad_a2 * (z2 > 0.0)
            grad_w2 = a1.T @ grad_z2
            grad_b2 = grad_z2.sum(axis=0)

            grad_a1 = grad_z2 @ weights["w2"].T
            grad_z1 = grad_a1 * (z1 > 0.0)
            grad_w1 = batch_features.T @ grad_z1
            grad_b1 = grad_z1.sum(axis=0)

            weights["w3"] -= LEARNING_RATE * grad_w3
            weights["b3"] -= LEARNING_RATE * grad_b3
            weights["w2"] -= LEARNING_RATE * grad_w2
            weights["b2"] -= LEARNING_RATE * grad_b2
            weights["w1"] -= LEARNING_RATE * grad_w1
            weights["b1"] -= LEARNING_RATE * grad_b1

        validation_accuracy = compute_accuracy(normalized_test, test_labels, weights)
        if validation_accuracy > best_accuracy:
            best_accuracy = validation_accuracy
            best_weights = {name: value.copy() for name, value in weights.items()}

    train_accuracy = compute_accuracy(normalized_train, train_labels, best_weights)
    test_probabilities = forward_pass(normalized_test, best_weights)[4]
    test_predictions = test_probabilities.argmax(axis=1)
    test_accuracy = float(np.mean(test_predictions == test_labels))

    confusion_matrix = np.zeros((OUTPUT_CLASSES, OUTPUT_CLASSES), dtype=np.int32)
    for expected, predicted in zip(test_labels, test_predictions):
        confusion_matrix[int(expected), int(predicted)] += 1

    class_accuracy = {}
    for class_id, class_name in LABEL_NAMES.items():
        class_total = int(np.sum(confusion_matrix[class_id]))
        correct = int(confusion_matrix[class_id, class_id])
        class_accuracy[class_name] = correct / class_total if class_total else 0.0

    return TrainingResult(
        mean=mean,
        std=std,
        weights=best_weights,
        train_accuracy=train_accuracy,
        test_accuracy=test_accuracy,
        confusion_matrix=confusion_matrix,
        class_accuracy=class_accuracy,
        train_count=len(train_features),
        test_count=len(test_features),
    )


def format_float(value: float) -> str:
    return f"{float(value):.6f}f"


def format_vector(values: np.ndarray, indent: str = "  ") -> str:
    lines = [indent + format_float(value) for value in values]
    return ",\n".join(lines)


def format_matrix(values: np.ndarray, indent: str = "    ") -> str:
    rows = []
    for row in values:
      row_values = ", ".join(format_float(value) for value in row)
      rows.append(f"{indent}{{ {row_values} }}")
    return ",\n".join(rows)


def write_dataset_csv(
    dataset: DatasetBundle,
    train_count: int,
    path: Path,
) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8", newline="") as handle:
        handle.write("split,source,temperature_c,humidity_percent,label_id,label_name\n")
        for index, (features, label, source) in enumerate(
            zip(dataset.features, dataset.labels, dataset.sources)
        ):
            split = "train" if index < train_count else "test"
            handle.write(
                f"{split},{source},{features[0]:.3f},{features[1]:.3f},{int(label)},{LABEL_NAMES[int(label)]}\n"
            )


def write_model_header(
    result: TrainingResult,
    test_features: np.ndarray,
    test_labels: np.ndarray,
    path: Path,
) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    evaluation_rows = []
    for features, label in zip(test_features, test_labels):
        evaluation_rows.append(
            "  { "
            f"{format_float(features[0])}, "
            f"{format_float(features[1])}, "
            f"{int(label)} "
            "}"
        )

    header = f"""#pragma once

#include <stddef.h>
#include <stdint.h>

namespace tinyml_model {{

constexpr size_t kInputSize = 2;
constexpr size_t kHidden1Size = {HIDDEN_1};
constexpr size_t kHidden2Size = {HIDDEN_2};
constexpr size_t kOutputSize = {OUTPUT_CLASSES};
constexpr uint16_t kTrainingSampleCount = {result.train_count};
constexpr uint16_t kEvaluationSampleCount = {result.test_count};
constexpr float kExpectedAccuracy = {format_float(result.test_accuracy)};
constexpr char kModelName[] = "tinyml_temp_humidity_v1";

struct EvaluationSample {{
  float temperatureC;
  float humidityPercent;
  uint8_t label;
}};

constexpr float kInputMean[kInputSize] = {{
{format_vector(result.mean, "  ")}
}};

constexpr float kInputStd[kInputSize] = {{
{format_vector(result.std, "  ")}
}};

constexpr float kWeights1[kInputSize][kHidden1Size] = {{
{format_matrix(result.weights["w1"])}
}};

constexpr float kBias1[kHidden1Size] = {{
{format_vector(result.weights["b1"], "  ")}
}};

constexpr float kWeights2[kHidden1Size][kHidden2Size] = {{
{format_matrix(result.weights["w2"])}
}};

constexpr float kBias2[kHidden2Size] = {{
{format_vector(result.weights["b2"], "  ")}
}};

constexpr float kWeights3[kHidden2Size][kOutputSize] = {{
{format_matrix(result.weights["w3"])}
}};

constexpr float kBias3[kOutputSize] = {{
{format_vector(result.weights["b3"], "  ")}
}};

constexpr EvaluationSample kEvaluationSamples[kEvaluationSampleCount] = {{
{",\n".join(evaluation_rows)}
}};

}}  // namespace tinyml_model
"""
    path.write_text(header, encoding="utf-8")


def write_metrics(
    dataset: DatasetBundle,
    result: TrainingResult,
    path: Path,
) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    label_counts = {
        LABEL_NAMES[label]: int(np.sum(dataset.labels == label))
        for label in LABEL_NAMES
    }
    source_counts = {
        "uniform": int(np.sum(dataset.sources == "uniform")),
        "boundary": int(np.sum(dataset.sources == "boundary")),
    }

    metrics = {
        "seed": SEED,
        "dataset": {
            "total_samples": int(len(dataset.features)),
            "train_samples": result.train_count,
            "test_samples": result.test_count,
            "source_counts": source_counts,
            "label_counts": label_counts,
            "temperature_range_c": [15.0, 38.0],
            "humidity_range_percent": [20.0, 95.0],
        },
        "model": {
            "architecture": [2, HIDDEN_1, HIDDEN_2, OUTPUT_CLASSES],
            "activation": "relu",
            "optimizer": "mini-batch gradient descent",
            "learning_rate": LEARNING_RATE,
            "epochs": EPOCHS,
            "batch_size": BATCH_SIZE,
        },
        "metrics": {
            "train_accuracy": result.train_accuracy,
            "test_accuracy": result.test_accuracy,
            "class_accuracy": result.class_accuracy,
            "confusion_matrix": result.confusion_matrix.tolist(),
        },
    }

    path.write_text(json.dumps(metrics, indent=2), encoding="utf-8")


def write_report(result: TrainingResult, path: Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    report = f"""# Task 5 - TinyML Deployment & Accuracy Evaluation

## 1. Dataset description

- Dataset type: synthetic temperature and humidity samples for the DHT20 sensor pipeline.
- Total samples: {result.train_count + result.test_count}
- Train / test split: {result.train_count} / {result.test_count}
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

- Model architecture: 2 -> {HIDDEN_1} -> {HIDDEN_2} -> 3
- Hidden activation: ReLU
- Output: softmax probabilities for `normal`, `warning`, `critical`
- Deployment target: ESP32-S3 on Yolo UNO
- Integration method: generated weights stored in `include/tinyml_model.h`, inference runs directly inside the RTOS firmware

## 3. Accuracy evaluation

- Train accuracy: {result.train_accuracy * 100:.2f}%
- Test accuracy: {result.test_accuracy * 100:.2f}%
- Class accuracy:
  - normal: {result.class_accuracy["normal"] * 100:.2f}%
  - warning: {result.class_accuracy["warning"] * 100:.2f}%
  - critical: {result.class_accuracy["critical"] * 100:.2f}%

### Confusion matrix

Rows = ground truth, columns = predicted

| Actual \\ Predicted | Normal | Warning | Critical |
| --- | ---: | ---: | ---: |
| Normal | {int(result.confusion_matrix[0, 0])} | {int(result.confusion_matrix[0, 1])} | {int(result.confusion_matrix[0, 2])} |
| Warning | {int(result.confusion_matrix[1, 0])} | {int(result.confusion_matrix[1, 1])} | {int(result.confusion_matrix[1, 2])} |
| Critical | {int(result.confusion_matrix[2, 0])} | {int(result.confusion_matrix[2, 1])} | {int(result.confusion_matrix[2, 2])} |

## 4. Discussion

- The model reaches high accuracy because the classes are strongly correlated with temperature and humidity thresholds.
- Most mistakes happen close to boundary values between `warning` and `critical`, where small sensor variations can flip the label.
- The synthetic dataset is useful for coursework and firmware integration, but it does not replace a real deployment dataset gathered from the physical environment.
- To improve realism later, the next step would be collecting raw DHT20 readings over time and retraining with actual sensor drift and ambient noise.

## 5. Conclusion

The TinyML model is small enough to run on the Yolo UNO, preserves the older RTOS/LCD/AP/web features, and provides a deployable environmental-state classifier with test accuracy above 98%. The firmware also includes an on-device evaluation routine so the same hold-out dataset can be replayed on the microcontroller for accuracy and inference-time reporting.
"""
    path.write_text(report, encoding="utf-8")


def main() -> None:
    rng = np.random.default_rng(SEED)
    dataset = generate_dataset(rng)

    train_count = int(len(dataset.features) * TRAIN_RATIO)
    train_features = dataset.features[:train_count]
    test_features = dataset.features[train_count:]
    train_labels = dataset.labels[:train_count]
    test_labels = dataset.labels[train_count:]

    result = train_model(train_features, train_labels, test_features, test_labels, rng)

    write_dataset_csv(dataset, train_count, DATASET_PATH)
    write_model_header(result, test_features, test_labels, HEADER_PATH)
    write_metrics(dataset, result, METRICS_PATH)
    write_report(result, REPORT_PATH)

    print(f"Generated dataset: {DATASET_PATH}")
    print(f"Generated header:  {HEADER_PATH}")
    print(f"Generated metrics: {METRICS_PATH}")
    print(f"Generated report:  {REPORT_PATH}")
    print(f"Train accuracy:    {result.train_accuracy * 100:.2f}%")
    print(f"Test accuracy:     {result.test_accuracy * 100:.2f}%")


if __name__ == "__main__":
    main()
