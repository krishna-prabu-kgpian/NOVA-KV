| variant | readers | sampled loads | HITM loads | contended lines | top line: % of HITM | top line is the store lock |
|---|---:|---:|---:|---:|---:|---:|
| global\_lock | 1 | 10,882 | 302 | 33 | 34.8 | no |
| global\_lock | 2 | 15,052 | 640 | 41 | 42.8 | no |
| global\_lock | 4 | 22,368 | 1,663 | 93 | 48.0 | no |
| global\_lock | 8 | 37,576 | 3,482 | 175 | 29.9 | no |
| rwlock | 1 | 7,641 | 100 | 43 | 22.0 | yes |
| rwlock | 2 | 12,228 | 263 | 47 | 54.0 | no |
| rwlock | 4 | 21,239 | 559 | 85 | 44.7 | yes |
| rwlock | 8 | 42,656 | 1,093 | 73 | 67.0 | yes |
