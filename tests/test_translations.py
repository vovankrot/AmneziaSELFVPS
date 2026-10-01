"""Check both shipped UI catalogs, including formatting arguments and plurals."""
import pathlib
import re
import unittest
import xml.etree.ElementTree as ET

ROOT = pathlib.Path(__file__).resolve().parents[1]


class TranslationCatalogTests(unittest.TestCase):
    def test_complete_catalogs_and_format_arguments(self):
        for locale, forms in [('ru_RU', 3), ('en_US', 2)]:
            catalog = ET.parse(ROOT / f'client/translations/amneziavpn_{locale}.ts')
            for context in catalog.getroot().findall('context'):
                for message in context.findall('message'):
                    source = message.findtext('source') or ''
                    translation = message.find('translation')
                    if translation is not None and translation.get('type') in ('obsolete', 'vanished'):
                        continue
                    if not source.strip():
                        continue
                    with self.subTest(locale=locale, context=context.findtext('name'), source=source):
                        self.assertIsNotNone(translation)
                        if translation is None:
                            continue
                        self.assertIsNone(translation.get('type'))
                        values = translation.findall('numerusform') if message.get('numerus') == 'yes' else [translation]
                        if message.get('numerus') == 'yes':
                            self.assertEqual(len(values), forms)
                        expected = sorted(re.findall(r'%L?\d+|%n', source))
                        for value in values:
                            text = value.text or ''
                            self.assertTrue(text.strip())
                            self.assertEqual(sorted(re.findall(r'%L?\d+|%n', text)), expected)
                            if locale == 'en_US':
                                self.assertNotRegex(text, r'[А-Яа-яЁё]')


if __name__ == '__main__':
    unittest.main()
