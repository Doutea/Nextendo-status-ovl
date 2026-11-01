import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location('embed', Path(__file__).resolve().parents[1] / 'scripts/embed_relay.py')
embed = importlib.util.module_from_spec(spec)
spec.loader.exec_module(embed)

class EmbedTests(unittest.TestCase):
    def test_valid_config(self):
        text = embed.render('a' * 64)
        self.assertIn('https://relay.doutea.cloud', text)
        self.assertIn('relay_ip_backup=172.67.219.244', text)
        self.assertIn('relay_key=' + 'a' * 64, text)
        self.assertEqual(len(text.splitlines()), 2)

    def test_missing_invalid_or_injected_key(self):
        for key in ['', 'short', 'a' * 129, 'a' * 32 + '\n', 'a' * 32 + '"', 'a' * 32 + '\\']:
            with self.subTest(key_length=len(key)):
                with self.assertRaises(ValueError):
                    embed.render(key)

if __name__ == '__main__':
    unittest.main()
