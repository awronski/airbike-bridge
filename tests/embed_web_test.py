from pathlib import Path
import gzip
import re
import shutil
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1]

class EmbedTests(unittest.TestCase):
    def test_firmware_id_changes_with_pinned_build_configuration(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory)
            project=root/'project';build=root/'build'
            project.mkdir()
            shutil.copytree(ROOT/'web',project/'web')
            (project/'scripts').mkdir()
            script=ROOT/'scripts/embed_web.py'
            shutil.copyfile(script,project/'scripts/embed_web.py')
            config=project/'platformio.ini'
            config.write_bytes((ROOT/'platformio.ini').read_bytes())

            class Env:
                def subst(self,key):
                    return str(project if key=='$PROJECT_DIR' else build)
                def Append(self,**kwargs):
                    pass

            code=compile(script.read_text(),'embed_web.py','exec')
            build_id=build/'generated/diagnostics_build.h'
            exec(code,{'Import':lambda _:None,'env':Env()})
            before=build_id.read_text()
            config.write_text(config.read_text().replace('NimBLE-Arduino@2.5.2','NimBLE-Arduino@2.5.3'))
            exec(code,{'Import':lambda _:None,'env':Env()})
            self.assertNotEqual(build_id.read_text(),before)

    def test_scripts_and_fallback_media_are_embedded_for_offline_firmware(self):
        with tempfile.TemporaryDirectory() as directory:
            class Env:
                def subst(self,key):
                    return str(ROOT) if key=='$PROJECT_DIR' else directory
                def Append(self,**kwargs):
                    pass
            namespace={'Import':lambda name:None,'env':Env()}
            exec(compile((ROOT/'scripts/embed_web.py').read_text(),'embed_web.py','exec'),namespace)
            header=(Path(directory)/'generated/web_page.h').read_text()
            compressed=bytes(int(x,16) for x in re.findall(r'0x([0-9a-f]{2})',header))
            page=gzip.decompress(compressed).decode()
            self.assertLess(len(compressed),len(page.encode())//2)
            self.assertNotIn('<script src=',page)
            self.assertIn('function initScreenAwake()',page)
            self.assertIn('data:video/mp4;base64,',page)
            self.assertIn('NoSleep.js v0.12.0',page)
            original = (ROOT/'web/index.html').read_text()
            expected = re.sub(r'<script src="/([^"]+)"></script>',
                              lambda match: '<script>\n'+(ROOT/'web'/match[1]).read_text()+'\n</script>',
                              original)
            self.assertEqual(page,expected)

if __name__=='__main__':
    unittest.main()
