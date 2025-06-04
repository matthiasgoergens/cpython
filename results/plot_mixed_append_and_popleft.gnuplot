set terminal png size 2000,2000
set output 'mixed_append_and_popleft.png'


set style data points
set style line 1 lc rgb '#FF0000' pt 7 ps 0.5  # Red points for meque
set style line 2 lc rgb '#0000FF' pt 7 ps 0.5  # Blue points for deque

# Set up the fit functions
meque_f(x) = meque_a*x + meque_b  # Linear fit for meque
deque_f(x) = deque_a*x + deque_b  # Linear fit for deque

# Fit the data
fit meque_f(x) 'mixed_append_and_popleft.meque.data' using 2:3 via meque_a,meque_b
fit deque_f(x) 'mixed_append_and_popleft.deque.data' using 2:3 via deque_a,deque_b


# Add fit parameters to plot
set label 1 sprintf("meque: y = %.2e x + %.2e", meque_a, meque_b) at graph 0.05,0.95
set label 2 sprintf("deque: y = %.2e x + %.2e", deque_a, deque_b) at graph 0.05,0.90


# Plot the data and fits
plot meque_f(x) title 'meque fit' with lines ls 1 lw 3 dt 1, \
     'mixed_append_and_popleft.meque.data' using 2:3 title 'meque' with points ls 1 pt 7 ps 0.1, \
     deque_f(x) title 'deque fit' with lines ls 2 lw 3 dt 2, \
     'mixed_append_and_popleft.deque.data' using 2:3 title 'deque' with points ls 2 pt 7 ps 0.1

# Set labels and title
set xlabel 'Number of elements'
set ylabel 'Time (seconds)'
set title 'Performance Comparison of meque, deque, and list'

# Print fit parameters to terminal
print "meque: a = ", meque_a, ", b = ", meque_b
print "deque: a = ", deque_a, ", b = ", deque_b
