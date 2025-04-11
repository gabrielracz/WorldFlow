// float cellSize = (sg.cellSize);
// voxelMin = gridMin + vec3(mapPos) * cellSize;
// voxelMax = voxelMin + cellSize;

// vec3 scaledTDelta = tDelta / ((subgridLevel+1.0) + subgridLevel * 2.0);

// for (int j = 0; j < 3; j++) {
//     if (step[j] != 0.0) {
//         float nextBoundary = (step[j] > 0.0) ? voxelMax[j] : voxelMin[j];
//         float tNext = (nextBoundary - rayOrigin[j]) / rayDir[j];
//         if (tNext > t) {
//             tMaxAxis[j] = tNext;
//         } else {
//             // We're already past this boundary, find the next one
//             tMaxAxis[j] = t + scaledTDelta[j]; // Half the delta for higher res
//         }
//     }
// }