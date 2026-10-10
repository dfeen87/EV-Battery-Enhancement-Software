import sys
import os

# Ensure we can import the extension
sys.path.insert(0, os.path.abspath('ds_core/lib'))

try:
    import ds_enhancer_pybind
except ImportError as e:
    print(f"Failed to import ds_enhancer_pybind: {e}")
    sys.exit(1)

def test_diagnostic_report():
    print("Testing ds_enhancer_pybind.DiagnosticReport...")
    try:
        report = ds_enhancer_pybind.DiagnosticReport()

        # Default values should be false for booleans
        assert not report.soc_warning, "Expected soc_warning to be False by default"
        assert not report.low_temp_warning, "Expected low_temp_warning to be False by default"

        # Verify settable
        report.soc_warning = True
        report.low_temp_warning = True

        assert report.soc_warning, "Expected soc_warning to be True after setting"
        assert report.low_temp_warning, "Expected low_temp_warning to be True after setting"

        print("DiagnosticReport bindings are working correctly.")
    except AttributeError as e:
        print(f"Attribute error during test: {e}")
        sys.exit(1)
    except AssertionError as e:
        print(f"Assertion failed: {e}")
        sys.exit(1)
    except Exception as e:
        print(f"Unexpected error: {e}")
        sys.exit(1)

if __name__ == '__main__':
    test_diagnostic_report()
