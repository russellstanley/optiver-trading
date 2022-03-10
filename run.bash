# Reset the logs
rm autotrader.log

# Build and run
cmake -DCMAKE_BUILD_TYPE=Release -B build
cmake --build build --config Release
cp build/autotrader . 
python3 rtg.py run autotrader
rm autotrader