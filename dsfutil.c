#include <inttypes.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// Struct packing macros per compiler
#ifdef _MSC_VER
#define PACKED_STRUCT __pragma(pack(push, 1)) struct
#else
#define PACKED_STRUCT struct __attribute__((packed))
#endif

#ifdef _MSC_VER
#define END_PACKED __pragma(pack(pop))
#else
#define END_PACKED
#endif

// Data block size
#define BLOCK_SIZE 4096

// DSD chunk
typedef PACKED_STRUCT
{
    char     header[4];
    uint64_t length;
    uint64_t file_size;
    uint64_t metadata_ptr;
} dsd_chunk_t;
END_PACKED;

// fmt chunk
typedef PACKED_STRUCT
{
    char     header[4];
    uint64_t length;
    uint32_t version;
    uint32_t id;
    uint32_t channel_type;
    uint32_t num_channels;
    uint32_t sample_rate;
    uint32_t bits_per_sample;
    uint64_t num_samples;
    uint32_t block_size;
    uint32_t reserved;
} fmt_chunk_t;
END_PACKED;

// data chunk (header only - excluding data)
typedef PACKED_STRUCT
{
    char     header[4];
    uint64_t length;
} data_chunk_t;
END_PACKED;

// File header - encompasses DSD, fmt and data chunks
typedef PACKED_STRUCT
{
    dsd_chunk_t  dsd_chunk;
    fmt_chunk_t  fmt_chunk;
    data_chunk_t data_chunk;
} dsf_header_t;
END_PACKED;

// Enum for file operations
typedef enum
{
    OP_INVALID = -1,
    OP_SWAP_CHANNELS,
    OP_LEFT_ISOLATE,
    OP_RIGHT_ISOLATE,
    OP_LEFT_INVERT,
    OP_RIGHT_INVERT,
    OP_INVERT,
    NUM_OPERATIONS
} operation_t;

// Enum for string table
enum
{
    STR_KEYWORD,
    STR_DESCRIPTION,
    STR_FEEDBACK
};

// Operation string table - keyword, description, feedback 
const char *operations[][NUM_OPERATIONS] =
{
    { "swapch",   "Swaps left and right channel data",                   "Swapping channel order"  },
    { "lisolate", "Isolates left channel; copies data to right channel", "Isolating left channel"  },
    { "risolate", "Isolates right channel; copies data to left channel", "Isolating right channel" },
    { "linvert",  "Inverts left channel phase",                          "Inverting left channel"  },
    { "rinvert",  "Inverts right channel phase",                         "Inverting right channel" },
    { "invert",   "Inverts left and right channel phase",                "Inverting phase"         }
};

int main(int argc, char **argv)
{
    dsf_header_t header;
    FILE        *in, *out;
    operation_t  operation;
    uint8_t      blockL[BLOCK_SIZE], blockR[BLOCK_SIZE], *metadata_chunk;
    uint64_t     i, j, metadata_length, num_blocks;

    // Ensure valid number of arguments
    if (argc != 4)
    {
        printf("Usage: %s operation input.dsf output.dsf\n\n"
               "Valid operations:\n", argv[0]);

        // Print list of valid operations and their descriptions
        for (i = 0; i < NUM_OPERATIONS; i++)
            printf("\t- %s:\t%s\n", operations[i][STR_KEYWORD], operations[i][STR_DESCRIPTION]);

        return 0;
    }

    // Attempt to read in operation
    for (operation = OP_INVALID, i = OP_SWAP_CHANNELS; i < NUM_OPERATIONS; i++)
    {
        if (strcmp(argv[1], operations[i][STR_KEYWORD]) == 0)
            operation = i;
    }

    // Ensure operation valid
    if (operation == OP_INVALID)
    {
        fprintf(stderr, "Error: Invalid operation '%s'.\n", argv[1]);

        goto operation_error;
    }

    //
    // Open files
    //

    // Attempt to open input file for reading
    if ((in = fopen(argv[2], "rb")) == NULL)
    {
        perror("Error opening input file");

        return 1;
    }

    // Attempt to open output file for writing
    if ((out = fopen(argv[3], "wb")) == NULL)
    {
        perror("Error opening output file");

        goto output_error;
    }

    // 
    // Read input file
    //

    // Attempt to read file header
    if (fread(&header, 1, sizeof(dsf_header_t), in) != sizeof(dsf_header_t))
    {
        fputs("Error: Incomplete DSF header.\n", stderr);

        goto read_error;
    }

    // Verify DSD chunk header
    if (strncmp(header.dsd_chunk.header, "DSD ", sizeof(header.dsd_chunk.header)) != 0)
    {
        fprintf(stderr, "Error: Invalid header for DSD chunk '%c%c%c%c'.\n",
            header.dsd_chunk.header[0], header.dsd_chunk.header[1], header.dsd_chunk.header[2], header.dsd_chunk.header[3]);

        goto read_error;
    }

    // Verify DSD chunk length
    if (header.dsd_chunk.length != sizeof(dsd_chunk_t))
    {
        fprintf(stderr, "Error: Invalid length for DSD chunk %" PRIu64 ".\n", header.dsd_chunk.length);

        goto read_error;
    }

    // Verify fmt chunk header
    if (strncmp(header.fmt_chunk.header, "fmt ", sizeof(header.fmt_chunk.header)) != 0)
    {
        fprintf(stderr, "Error: Invalid header for fmt chunk '%c%c%c%c'.\n",
            header.fmt_chunk.header[0], header.fmt_chunk.header[1], header.fmt_chunk.header[2], header.fmt_chunk.header[3]);

        goto read_error;
    }

    // Verify fmt chunk length
    if (header.fmt_chunk.length != sizeof(fmt_chunk_t))
    {
        fprintf(stderr, "Error: Invalid length for fmt chunk %" PRIu64 ".\n", header.fmt_chunk.length);

        goto read_error;
    }

    // Verify other fmt chunk properties
    if (header.fmt_chunk.version                != 1          || header.fmt_chunk.id              != 0  ||
       (header.fmt_chunk.sample_rate % 2822400) != 0          ||
       (header.fmt_chunk.bits_per_sample        != 1          && header.fmt_chunk.bits_per_sample != 8) ||
        header.fmt_chunk.block_size             != BLOCK_SIZE || header.fmt_chunk.reserved        != 0)
    {
        fputs("Error: Invalid DSF file.\n", stderr);

        goto read_error;
    }
    
    // Verify data chunk header
    if (strncmp(header.data_chunk.header, "data", sizeof(header.data_chunk.header)) != 0)
    {
        fprintf(stderr, "Error: Invalid header for data chunk '%c%c%c%c'.\n",
            header.data_chunk.header[0], header.data_chunk.header[1], header.data_chunk.header[2], header.data_chunk.header[3]);

        goto read_error;
    }

    // Ensure input file is stereo
    if (header.fmt_chunk.channel_type != 2 || header.fmt_chunk.num_channels != 2)
    {
        fprintf(stderr, "Error: Invalid number of channels %" PRIu32 ".\n", header.fmt_chunk.num_channels);

        goto read_error;
    }

    // Print feedback
    printf("%s in %s - saving to %s...\n", operations[operation][STR_FEEDBACK], argv[2], argv[3]);

    // Read in metadata if present
    if (header.dsd_chunk.metadata_ptr != 0)
    {
        // Calculate metadata chunk length
        metadata_length = header.dsd_chunk.file_size - header.dsd_chunk.metadata_ptr;

        // Allocate memory for metadata chunk
        metadata_chunk = malloc(metadata_length);

        // Seek to, and read in metadata
        fseek(in, header.dsd_chunk.metadata_ptr, SEEK_SET);
        fread(metadata_chunk, 1, metadata_length, in);

        // Seek back to start of data
        fseek(in, sizeof(dsf_header_t), SEEK_SET);
    }
    else
    {
        metadata_chunk  = NULL;
        metadata_length = 0;
    }

    //
    // Write output file
    //

    // Write header
    fwrite(&header, 1, sizeof(dsf_header_t), out);

    // Calculate number of blocks
    num_blocks = (header.data_chunk.length - sizeof(data_chunk_t)) / BLOCK_SIZE;

    // Read and write blocks
    for (i = 0; i < num_blocks; i += 2)
    {
        // Read in block pair
        fread(blockL, 1, BLOCK_SIZE, in);
        fread(blockR, 1, BLOCK_SIZE, in);

        switch (operation)
        {
            case OP_SWAP_CHANNELS:
                // Write block pair in alternate order (thus swapping channels)
                fwrite(blockR, 1, BLOCK_SIZE, out);
                fwrite(blockL, 1, BLOCK_SIZE, out);

                continue;
            case OP_LEFT_ISOLATE:
                // Write left channel block to channel pair
                fwrite(blockL, 1, BLOCK_SIZE, out);
                fwrite(blockL, 1, BLOCK_SIZE, out);

                continue;
            case OP_RIGHT_ISOLATE:
                // Write right channel block to channel pair
                fwrite(blockR, 1, BLOCK_SIZE, out);
                fwrite(blockR, 1, BLOCK_SIZE, out);

                continue;
            case OP_LEFT_INVERT:
                // Invert left channel phase
                for (j = 0; j < BLOCK_SIZE; j++)
                    blockL[j] = ~blockL[j];

                break;
            case OP_RIGHT_INVERT:
                // Invert right channel phase
                for (j = 0; j < BLOCK_SIZE; j++)
                    blockR[j] = ~blockR[j];

                break;
            case OP_INVERT:
                // Invert left and right channel phase
                for (j = 0; j < BLOCK_SIZE; j++)
                {
                    blockL[j] = ~blockL[j];
                    blockR[j] = ~blockR[j];
                }
        }

        // Write block pair
        fwrite(blockL, 1, BLOCK_SIZE, out);
        fwrite(blockR, 1, BLOCK_SIZE, out);
    }

    // Write and de-allocate metadata chunk if present
    if (metadata_chunk)
    {
        fwrite(metadata_chunk, 1, metadata_length, out);
        free(metadata_chunk);
    }

    // Close files
    fclose(in);
    fclose(out);

    puts("Done!");

    return 0;

    // Close file objects and return error code
read_error:
    fclose(out);

output_error:
    fclose(in);

operation_error:
    return 1;
}
