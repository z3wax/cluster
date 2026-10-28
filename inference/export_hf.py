#!/usr/bin/env python3
"""Export local Hugging Face causal LM checkpoints to CPU pipeline stage shards.

Supports GPT2, Llama, Mistral and Qwen2 dense checkpoints, saved by Transformers
as Safetensors or PyTorch weights. No GGUF conversion. Each stage contains only
its own embeddings/blocks/head. Inference uses full unpadded contexts without a
KV cache; models requiring custom Python code or quantization are not accepted.
"""
import argparse
import json
from pathlib import Path
import sys
import tempfile
from urllib.parse import urlsplit

SUPPORTED = {"gpt2", "llama", "mistral", "qwen2"}


def export_model(source, output, workers, example_length=8, full_logits=False):
    """Return the generated pipeline path, after checking split/unsplit parity."""
    import torch
    from transformers import AutoConfig, AutoModelForCausalLM, AutoTokenizer

    source = Path(source).expanduser().resolve()
    output = Path(output).expanduser().resolve()
    if not source.is_dir():
        raise ValueError("--model must be a local Hugging Face checkpoint directory")
    if len(workers) < 2:
        raise ValueError("at least two workers are required")
    workers = [worker.rstrip("/") for worker in workers]
    if len(set(workers)) != len(workers):
        raise ValueError("worker addresses must be distinct")
    for worker in workers:
        parsed = urlsplit(worker)
        if (parsed.scheme not in ("http", "https") or not parsed.hostname
                or any(character.isspace() for character in parsed.hostname)
                or parsed.path not in ("", "/") or parsed.query or parsed.fragment
                or parsed.username or parsed.password):
            raise ValueError("worker addresses must be http(s)://HOST:PORT URLs")
        _ = parsed.port  # Reject invalid ports before loading checkpoint weights.
    if example_length < 2:
        raise ValueError("example length must be at least 2")
    if output.exists() and any(output.iterdir()):
        raise ValueError("output directory must be empty")
    config = AutoConfig.from_pretrained(str(source), local_files_only=True, trust_remote_code=False)
    if config.model_type not in SUPPORTED:
        raise ValueError(f"unsupported model architecture: {config.model_type}; supported: {', '.join(sorted(SUPPORTED))}")
    if getattr(config, "quantization_config", None):
        raise ValueError("quantized Hugging Face checkpoints require their own runtime adapter")
    rope = getattr(config, "rope_scaling", None) or {}
    rope_type = str(rope.get("rope_type", rope.get("type", "")))
    if "dynamic" in rope_type or rope_type == "longrope":
        raise ValueError("dynamic RoPE and LongRoPE are not supported by this TorchScript exporter")
    model = AutoModelForCausalLM.from_pretrained(
        str(source), local_files_only=True, trust_remote_code=False,
        attn_implementation="eager", torch_dtype=torch.float32,
    ).cpu().eval()
    gpt2 = config.model_type == "gpt2"
    base = model.transformer if gpt2 else model.model
    blocks = base.h if gpt2 else base.layers
    if len(workers) > len(blocks):
        raise ValueError("worker count cannot exceed transformer block count")
    max_length = getattr(config, "max_position_embeddings", getattr(config, "n_positions", 1024))
    if example_length + 3 > max_length:
        raise ValueError("example length must leave three positions within model context")
    boundaries = [len(blocks) * index // len(workers) for index in range(len(workers) + 1)]

    class Stage(torch.nn.Module):
        def __init__(self, index):
            super().__init__()
            self.first = index == 0
            self.last = index == len(workers) - 1
            self.blocks = torch.nn.ModuleList(list(blocks)[boundaries[index]:boundaries[index + 1]])
            if self.first:
                self.embeddings = base.wte if gpt2 else base.embed_tokens
                if gpt2:
                    self.positions = base.wpe
            self.sliding_window = getattr(config, "sliding_window", None)
            self.sliding = [self.sliding_window is not None and (
                config.model_type == "mistral" or getattr(block, "attention_type", "") == "sliding_attention"
            ) for block in self.blocks]
            if not gpt2:
                self.rotary = base.rotary_emb
            if self.last:
                self.norm = base.ln_f if gpt2 else base.norm
                self.head = model.lm_head

        def forward(self, value):
            if self.first:
                hidden = self.embeddings(value)
            else:
                hidden = value
            length = hidden.shape[1]
            positions = torch.arange(length, device=hidden.device).unsqueeze(0)
            if self.first and gpt2:
                hidden = hidden + self.positions(positions)
            # Explicit dynamic causal mask avoids version-specific HF mask builders.
            mask = torch.full((length, length), torch.finfo(hidden.dtype).min,
                              dtype=hidden.dtype, device=hidden.device)
            mask = torch.triu(mask, diagonal=1)[None, None, :, :]
            if not gpt2:
                position_embeddings = self.rotary(hidden, positions)
            sliding_mask = mask
            if any(self.sliding):
                blocked = torch.tril(torch.ones((length, length), dtype=torch.bool, device=hidden.device), diagonal=-self.sliding_window)
                sliding_mask = mask.masked_fill(blocked[None, None, :, :], torch.finfo(hidden.dtype).min)
            for index, block in enumerate(self.blocks):
                layer_mask = sliding_mask if self.sliding[index] else mask
                if gpt2:
                    result = block(hidden, attention_mask=layer_mask, use_cache=False)
                else:
                    result = block(hidden, attention_mask=layer_mask, position_ids=positions,
                                   position_embeddings=position_embeddings, use_cache=False)
                hidden = result[0] if isinstance(result, tuple) else result
            if self.last:
                hidden = hidden if full_logits else hidden[:, -1:, :]
                return self.head(self.norm(hidden))
            return hidden

    shards = []
    value = torch.zeros((1, example_length), dtype=torch.int64)
    with torch.inference_mode():
        for index in range(len(workers)):
            stage = Stage(index).eval()
            traced = torch.jit.trace(stage, value, strict=True)
            shards.append(traced)
            value = stage(value)
        # Check an untraced sequence length and a different batch size too.
        for batch, length in ((1, example_length), (2, example_length + 3), (1, 1)):
            ids = torch.arange(batch * length).reshape(batch, length) % config.vocab_size
            expected = model(ids, use_cache=False).logits
            if not full_logits:
                expected = expected[:, -1:, :]
            actual = ids
            for shard in shards:
                actual = shard(actual)
            torch.testing.assert_close(actual, expected, rtol=2e-4, atol=2e-5)

    # Load optional tokenizer before writing artifacts, so invalid tokenizer files
    # cannot leave an apparently ready partial export behind.
    tokenizer_files = ("tokenizer.json", "tokenizer_config.json", "vocab.json", "tokenizer.model")
    tokenizer = None
    if any((source / filename).is_file() for filename in tokenizer_files):
        tokenizer = AutoTokenizer.from_pretrained(str(source), local_files_only=True, trust_remote_code=False)
    eos = getattr(model.generation_config, "eos_token_id", None)
    eos = [eos] if type(eos) is int else (eos or [])
    if not isinstance(eos, (list, tuple)) or any(type(token) is not int or token < 0 for token in eos):
        raise ValueError("generation config eos_token_id must be an integer or list of integers")
    eos = list(dict.fromkeys(eos))
    # Publish a complete export atomically. Disk errors or interrupted writes
    # must not leave partial shards in the destination and block a safe retry.
    destination = output
    destination.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".sdrive-export-", dir=destination.parent) as temporary:
        output = Path(temporary) / "artifacts"
        output.mkdir()
        pipeline = {"version": 1, "inputs": ["input_ids"], "outputs": ["logits"],
                    "max_sequence_length": max_length, "eos_token_ids": eos,
                    "logits": "full" if full_logits else "last-token", "stages": []}
        for index, (worker, shard) in enumerate(zip(workers, shards)):
            shard_path = output / f"stage-{index}.pt"
            shard.save(str(shard_path))
            stage_id = f"stage{index}"
            input_name = "input_ids" if index == 0 else "hidden_states"
            output_name = "logits" if index == len(workers) - 1 else "hidden_states"
            worker_config = {"version": 1, "stages": [{
                "id": stage_id, "engine": "torchscript", "model": shard_path.name,
                "inputs": [input_name], "outputs": [output_name],
            }]}
            (output / f"worker-{index}.json").write_text(json.dumps(worker_config, indent=2) + "\n")
            pipeline["stages"].append({"worker": worker.rstrip("/"), "stage": stage_id,
                                       "inputs": {input_name: input_name},
                                       "outputs": {output_name: output_name}})
        # Checkpoint directories without tokenizers remain usable through tensor API.
        if tokenizer is not None:
            tokenizer.save_pretrained(output / "tokenizer")
        pipeline_path = output / "pipeline.json"
        pipeline_path.write_text(json.dumps(pipeline, indent=2) + "\n")
        (output / "export.json").write_text(json.dumps({
            "architecture": config.model_type, "blocks": len(blocks), "boundaries": boundaries,
            "dtype": "float32", "max_sequence_length": max_length, "eos_token_ids": eos,
            "parity_checked": True, "padding": False, "kv_cache": False, "logits": "full" if full_logits else "last-token",
        }, indent=2) + "\n")
        if destination.exists():
            # rmdir only succeeds for an empty directory; never overwrite files
            # another process may have created since the initial validation.
            destination.rmdir()
        output.replace(destination)
    return destination / "pipeline.json"


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--model", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--workers", required=True, help="comma-separated http(s)://HOST:PORT URLs")
    parser.add_argument("--example-length", type=int, default=8)
    parser.add_argument("--full-logits", action="store_true", help="return all positions instead of last-token logits")
    args = parser.parse_args(argv)
    try:
        result = export_model(args.model, args.output, args.workers.split(","), args.example_length, args.full_logits)
        print(result)
        return 0
    except (ValueError, OSError, ImportError, AssertionError, RuntimeError) as exc:
        print(f"Export failed: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
