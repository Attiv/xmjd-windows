# Local contextual correction assets

The ONNX-format model and original metadata are byte-identical to
the Mac contextual-correction integration in Attiv/xmjd_ios at f7c5d6c501.
The Apache-2.0 license and corpus attributions accompany these files. The
vocab.tsv file lists the decimal Unicode scalar and original token index
for each one-scalar entry of the original vocab.json. Special tokens retain
their original indices: unknown is 1 and beginning of sequence is 2.

Windows builds obtain Microsoft ONNX Runtime 1.24.3 CPU binaries from NuGet
using the SHA-256 pinned in Windows/cmake/PrepareMsime.cmake. Its license and
third-party notices are installed beside the binaries.
