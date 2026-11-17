from pathlib import Path
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1]

class EmbedTests(unittest.TestCase):
    def test_scripts_and_fallback_media_are_embedded_for_offline_firmware(self):
        with tempfile.TemporaryDirectory() as directory:
            class Env:
                def subst(self,key):
                    return str(ROOT) if key=='$PROJECT_DIR' else directory
                def Append(self,**kwargs):
                    pass
            namespace={'Import':lambda name:None,'env':Env()}
            exec(compile((ROOT/'scripts/embed_web.py').read_text(),'embed_web.py','exec'),namespace)
            page=(Path(directory)/'generated/web_page.h').read_text()
            self.assertNotIn('<script src=',page)
            self.assertIn('function initScreenAwake()',page)
            self.assertIn('data:video/mp4;base64,',page)
            self.assertIn('NoSleep.js v0.12.0',page)

if __name__=='__main__':
    unittest.main()
