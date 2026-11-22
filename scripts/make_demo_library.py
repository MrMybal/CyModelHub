# SPDX-FileCopyrightText: 2026 Cyberalien
# SPDX-License-Identifier: GPL-3.0-only

"""Create a folder of tiny synthetic model files for trying CyModelHub without real models.

    python scripts/make_demo_library.py build/demo-library

Each file has a genuine header (GGUF metadata, safetensors tensor names) and no weights, so the
scanner classifies it exactly as it would the real thing. The files cannot be loaded by a model
runtime.
"""

import json
import pathlib
import struct
import sys


def gguf(path, architecture, name, file_type=15, chat=True, extra=()):
    def text(value):
        data = value.encode("utf-8")
        return struct.pack("<Q", len(data)) + data

    def pair(key, kind, payload):
        return text(key) + struct.pack("<I", kind) + payload

    pairs = [
        pair("general.architecture", 8, text(architecture)),
        pair("general.name", 8, text(name)),
        pair("general.file_type", 4, struct.pack("<I", file_type)),
        pair(f"{architecture}.context_length", 4, struct.pack("<I", 8192)),
    ]
    if chat:
        pairs.append(pair("tokenizer.ggml.model", 8, text("gpt2")))
        pairs.append(pair("tokenizer.chat_template", 8, text("{{ messages }}")))
    for key, value in extra:
        pairs.append(pair(key, 8, text(value)))
    body = b"GGUF" + struct.pack("<IQQ", 3, 0, len(pairs)) + b"".join(pairs)
    write(path, body + name.encode("utf-8") * 64 + bytes(4096))


def safetensors(path, tensors, metadata=None, content=None):
    """`content` seeds the fake weights: two files with the same seed are identical copies."""
    header = {name: {"dtype": "F16", "shape": [1, 1], "data_offsets": [0, 2]} for name in tensors}
    if metadata:
        header["__metadata__"] = metadata
    data = json.dumps(header).encode("utf-8")
    seed = (content or path.name).encode("utf-8")
    write(path, struct.pack("<Q", len(data)) + data + seed * 64 + bytes(4096))


def write(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)


def main():
    root = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else "demo-library")
    gguf(root / "llm" / "Nova-7B-Instruct.Q4_K_M.gguf", "llama", "Nova 7B Instruct")
    gguf(root / "llm" / "Lyra-3B-Chat.Q8_0.gguf", "qwen2", "Lyra 3B Chat", file_type=7)
    gguf(root / "llm" / "atlas-embed-text-v2.F16.gguf", "nomic-bert", "Atlas Embed", file_type=1, chat=False,
         extra=[("nomic-bert.pooling_type", "mean")])
    gguf(root / "diffusion_models" / "prisma-dev-Q5_K_M.gguf", "flux", "Prisma Dev", file_type=17, chat=False)
    sdxl = ["model.diffusion_model.input_blocks.0.0.weight", "model.diffusion_model.label_emb.0.0.weight",
            "conditioner.embedders.1.model.ln_final.weight", "first_stage_model.decoder.up.0.block.0.conv1.weight"]
    safetensors(root / "checkpoints" / "aurora-xl-v3.safetensors", sdxl)
    safetensors(root / "comfyui-copy" / "checkpoints" / "aurora-xl-v3.safetensors", sdxl,
                content="aurora-xl-v3.safetensors")
    safetensors(root / "checkpoints" / "classic-v15.safetensors",
                ["model.diffusion_model.input_blocks.0.0.weight", "cond_stage_model.transformer.text_model.x",
                 "first_stage_model.decoder.up.0.block.0.conv1.weight"])
    safetensors(root / "diffusion_models" / "motion-t2v-14B.safetensors",
                ["patch_embedding.weight", "blocks.0.cross_attn.k.weight", "text_embedding.0.weight"])
    safetensors(root / "loras" / "watercolour-style.safetensors",
                ["lora_unet_down_blocks_1.lora_up.weight", "lora_unet_down_blocks_1.lora_down.weight",
                 "lora_te2_text_model.lora_up.weight"],
                {"ss_base_model_version": "sdxl_base_v1-0", "ss_network_module": "networks.lora"})
    safetensors(root / "loras" / "prisma-film-grain.safetensors",
                ["lora_transformer_double_blocks_0.lora_A.weight", "lora_transformer_double_blocks_0.lora_B.weight"],
                {"modelspec.architecture": "flux-1-dev/lora"})
    safetensors(root / "vae" / "xl-vae-fix.safetensors",
                ["decoder.up.0.block.0.conv1.weight", "encoder.down.0.block.0.conv1.weight", "quant_conv.weight"])
    safetensors(root / "controlnet" / "depth-xl.safetensors",
                ["control_model.input_blocks.0.0.weight", "control_model.input_hint_block.0.weight"],
                {"modelspec.architecture": "stable-diffusion-xl-v1-base/controlnet"})
    safetensors(root / "text_encoders" / "t5xxl_fp16.safetensors", ["encoder.block.0.layer.0.SelfAttention.q.weight"])
    safetensors(root / "audio" / "stable-audio-sfx.safetensors", ["model.model.transformer.layers.0.weight"])
    write(root / "upscale_models" / "4x-UltraSharp.pth", b"PK\x03\x04" + bytes(8192))
    write(root / "whisper" / "ggml-base.en.bin", b"lmgg" + bytes(9 * 1024 * 1024))
    write(root / "pipelines" / "prisma-schnell" / "model_index.json",
          json.dumps({"_class_name": "FluxPipeline", "_diffusers_version": "0.31.0"}).encode())
    safetensors(root / "pipelines" / "prisma-schnell" / "transformer" / "diffusion_pytorch_model.safetensors",
                ["double_blocks.0.img_attn.qkv.weight"])
    print(f"Demo library written to {root.resolve()}")


if __name__ == "__main__":
    main()
