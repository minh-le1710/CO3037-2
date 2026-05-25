const demos = {
  "credit-1": {
    kind: "Credit 1",
    title: "LED cảnh báo nhiệt độ",
    description: "Demo hệ thống cảnh báo bằng LED dựa trên nhiệt độ đo được.",
    video: "./videos/credit-1.mp4",
  },
  "credit-3": {
    kind: "Credit 3",
    title: "OLED hiển thị NORMAL / WARNING / CRITICAL",
    description: "Demo hiển thị trạng thái hệ thống trên màn hình OLED.",
    video: "./videos/credit-3-5.mp4",
  },
  "credit-5": {
    kind: "Credit 5",
    title: "TinyML phân loại trạng thái hệ thống",
    description: "Demo suy luận TinyML, dùng chung video với phần credit 3-5.",
    video: "./videos/credit-3-5.mp4",
  },
  "credit-6": {
    kind: "Credit 6",
    title: "Publish dữ liệu lên CoreIOT Cloud Server",
    description: "Demo kết nối cloud và đồng bộ telemetry lên dashboard CoreIOT.",
    video: "./videos/credit-6.mp4",
  },
};

const params = new URLSearchParams(window.location.search);
const demoId = params.get("demo");
const data = demos[demoId];

const titleNode = document.getElementById("demo-title");
const kindNode = document.getElementById("demo-kind");
const descriptionNode = document.getElementById("demo-description");
const videoNode = document.getElementById("demo-video");

if (!data) {
  kindNode.textContent = "Không tìm thấy demo";
  titleNode.textContent = "Liên kết không hợp lệ";
  descriptionNode.textContent = "Hãy quay lại gallery và chọn một credit hợp lệ.";
  videoNode.remove();
} else {
  document.title = `${data.kind} - ${data.title}`;
  kindNode.textContent = data.kind;
  titleNode.textContent = data.title;
  descriptionNode.textContent = data.description;
  videoNode.src = data.video;
  videoNode.poster = "";
  videoNode.load();
}
