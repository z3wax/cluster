import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location("export_hf", Path(__file__).parents[1] / "export_hf.py")
export_hf = importlib.util.module_from_spec(spec)
spec.loader.exec_module(export_hf)

DEPENDENCIES = all(importlib.util.find_spec(name) for name in ("torch", "transformers"))


@unittest.skipUnless(DEPENDENCIES, "install torch and transformers to test HF export")
class HuggingFaceExportTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        import torch
        torch.set_num_threads(1)

    def check_export(self, model, safe_serialization=True, full_logits=False):
        import torch
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "source"
            model.eval().save_pretrained(source, safe_serialization=safe_serialization)
            from tokenizers import Tokenizer
            from tokenizers.models import WordLevel
            from transformers import PreTrainedTokenizerFast
            tokenizer = PreTrainedTokenizerFast(tokenizer_object=Tokenizer(WordLevel({"[UNK]": 0, "hello": 1}, unk_token="[UNK]")), unk_token="[UNK]")
            tokenizer.save_pretrained(source)
            output = Path(directory) / "export"
            path = export_hf.export_model(source, output, ["http://pc1:50052", "http://pc2:50052"], full_logits=full_logits)
            pipeline = json.loads(path.read_text())
            self.assertTrue((output / "tokenizer" / "tokenizer.json").is_file())
            expected_eos = model.generation_config.eos_token_id
            expected_eos = [expected_eos] if type(expected_eos) is int else (expected_eos or [])
            self.assertEqual(pipeline["eos_token_ids"], expected_eos)
            self.assertEqual(len(pipeline["stages"]), 2)
            self.assertEqual(pipeline["stages"][0]["worker"], "http://pc1:50052")
            ids = torch.tensor([[1, 2, 3, 4, 5]])
            with torch.inference_mode():
                result = ids
                for index in range(2):
                    result = torch.jit.load(str(output / f"stage-{index}.pt"))(result)
                expected = model(ids, use_cache=False).logits
                if not full_logits:
                    expected = expected[:, -1:, :]
                torch.testing.assert_close(result, expected, rtol=2e-4, atol=2e-5)
            # Worker artifacts contain only their own block range, not the full model.
            first = torch.jit.load(str(output / "stage-0.pt"))
            self.assertNotIn("head.weight", dict(first.named_parameters()))
            last = torch.jit.load(str(output / "stage-1.pt"))
            self.assertNotIn("embeddings.weight", dict(last.named_parameters()))

    def test_gpt2_safetensors(self):
        from transformers import GPT2Config, GPT2LMHeadModel
        self.check_export(GPT2LMHeadModel(GPT2Config(vocab_size=32, n_positions=32, n_embd=16, n_layer=4, n_head=2)))

    def test_gpt2_pytorch_weights(self):
        from transformers import GPT2Config, GPT2LMHeadModel
        self.check_export(GPT2LMHeadModel(GPT2Config(vocab_size=32, n_positions=32, n_embd=16, n_layer=2, n_head=2)), safe_serialization=False)

    def test_gpt2_full_logits(self):
        from transformers import GPT2Config, GPT2LMHeadModel
        self.check_export(GPT2LMHeadModel(GPT2Config(vocab_size=32, n_positions=32, n_embd=16, n_layer=2, n_head=2)), full_logits=True)

    def test_unsupported_architecture_rejected_before_weights(self):
        from transformers import BertConfig
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "source"
            BertConfig().save_pretrained(source)
            with self.assertRaisesRegex(ValueError, "unsupported model architecture"):
                export_hf.export_model(source, Path(directory) / "out", ["http://pc1:50052", "http://pc2:50052"])

    def test_invalid_worker_urls_rejected_before_checkpoint_loading(self):
        with tempfile.TemporaryDirectory() as directory:
            invalid = (["http://pc1:50052", "http://pc1:50052/"],
                       ["http://pc1:70000", "http://pc2:50052"],
                       ["http://pc1:bad", "http://pc2:50052"],
                       ["http://bad host:50052", "http://pc2:50052"])
            for workers in invalid:
                with self.subTest(workers=workers), self.assertRaises(ValueError):
                    export_hf.export_model(directory, Path(directory) / "out", workers)

    def test_longrope_rejected_before_checkpoint_weights(self):
        from transformers import LlamaConfig
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "source"
            LlamaConfig(hidden_size=16, num_attention_heads=2, num_key_value_heads=2,
                        max_position_embeddings=64, rope_scaling={
                            "rope_type": "longrope", "factor": 2.0,
                            "short_factor": [1.0] * 4, "long_factor": [2.0] * 4,
                        }).save_pretrained(source)
            with self.assertRaisesRegex(ValueError, "LongRoPE"):
                export_hf.export_model(source, Path(directory) / "out", ["http://pc1:50052", "http://pc2:50052"])

    def test_multiple_eos_token_ids_exported(self):
        from transformers import GPT2Config, GPT2LMHeadModel
        self.check_export(GPT2LMHeadModel(GPT2Config(vocab_size=32, n_positions=32, n_embd=16,
                                                  n_layer=2, n_head=2, eos_token_id=[1, 7])))

    def test_llama_safetensors(self):
        from transformers import LlamaConfig, LlamaForCausalLM
        self.check_export(LlamaForCausalLM(LlamaConfig(vocab_size=32, hidden_size=16, intermediate_size=32,
                                                      num_hidden_layers=4, num_attention_heads=2,
                                                      num_key_value_heads=2, max_position_embeddings=32)))

    def test_qwen2_safetensors(self):
        from transformers import Qwen2Config, Qwen2ForCausalLM
        self.check_export(Qwen2ForCausalLM(Qwen2Config(vocab_size=32, hidden_size=16, intermediate_size=32,
                                                      num_hidden_layers=4, num_attention_heads=2,
                                                      num_key_value_heads=2, max_position_embeddings=32)))

    def test_mistral_safetensors(self):
        from transformers import MistralConfig, MistralForCausalLM
        self.check_export(MistralForCausalLM(MistralConfig(vocab_size=32, hidden_size=16, intermediate_size=32,
                                                          num_hidden_layers=4, num_attention_heads=2,
                                                          num_key_value_heads=2, max_position_embeddings=32,
                                                          sliding_window=4)))


if __name__ == "__main__":
    unittest.main()
