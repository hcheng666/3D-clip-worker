variable "IMAGE" {
  default = "3d-tiles-clip-worker:dev"
}

group "default" {
  targets = ["worker"]
}

target "worker" {
  context = "."
  dockerfile = "Dockerfile"
  platforms = ["linux/amd64", "linux/arm64"]
  tags = [IMAGE]
}

