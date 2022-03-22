# Reset the logs
rm fancy.log

# Build and run
cmake -DCMAKE_BUILD_TYPE=Release -B build
cmake --build build --config Release
cp build/autotrader fancy
python3 rtg.py run basic fancy
rm autotrader